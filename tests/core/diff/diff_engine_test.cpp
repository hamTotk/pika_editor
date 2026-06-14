// core/diff DiffEngine（累積差分・大規模ガード・色非依存・キャンセル）の検証。sprint6 must/should。
// ベースライン内容→現在内容の累積差分、改行のみ差で空、行内強調の付与、大規模入力ガード
// （別スレッド中断に頼らない開始前判定）、協調キャンセルを観測する（要件8章 / design.md 8章）。
#include "core/diff/cancel_token.h"
#include "core/diff/diff_engine.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using pika::core::diff::CancelToken;
using pika::core::diff::DiffEngine;
using pika::core::diff::DiffLimits;
using pika::core::diff::DiffResult;
using pika::core::diff::LineOp;

TEST(DiffEngineTest, CumulativeDiffCountsAddedAndRemoved)
{
    DiffEngine engine;
    DiffResult r = engine.compute("a\nb\nc\n", "a\nB\nc\nd\n");
    EXPECT_FALSE(r.truncated);
    EXPECT_FALSE(r.cancelled);
    // b→B の変更（-1/+1）と d の追加（+1）。removed=1, added=2。
    EXPECT_EQ(r.removed, 1u);
    EXPECT_EQ(r.added, 2u);
}

TEST(DiffEngineTest, CrlfVsLfOnlyDiffIsEmpty)
{
    // 改行のみの差は差分に出ない（LF 正規化照合。要件8章）。
    DiffEngine engine;
    DiffResult r = engine.compute("x\r\ny\r\n", "x\ny\n");
    EXPECT_FALSE(r.truncated);
    EXPECT_EQ(r.added, 0u);
    EXPECT_EQ(r.removed, 0u);
}

TEST(DiffEngineTest, ChangedLinePairGetsInlineSpans)
{
    // 変更行ペア（delete+add）に行内強調が付く。
    DiffEngine engine;
    DiffResult r = engine.compute("hello world\n", "hello there\n");
    bool found_del_span = false;
    bool found_add_span = false;
    for (const auto& l : r.lines)
    {
        if (l.op == LineOp::Delete && !l.spans.empty())
        {
            found_del_span = true;
        }
        if (l.op == LineOp::Add && !l.spans.empty())
        {
            found_add_span = true;
        }
    }
    EXPECT_TRUE(found_del_span);
    EXPECT_TRUE(found_add_span);
}

TEST(DiffEngineTest, EveryLineHasColorIndependentMarker)
{
    DiffEngine engine;
    DiffResult r = engine.compute("keep\nold\nkeep2\n", "keep\nnew\nkeep2\n");
    ASSERT_FALSE(r.lines.empty());
    for (const auto& l : r.lines)
    {
        const char m = l.marker();
        EXPECT_TRUE(m == '+' || m == '-' || m == ' ');
    }
}

TEST(DiffEngineTest, LargeByteInputTruncatesBeforeComputing)
{
    // 片側がバイト上限超過なら計算を開始せず truncated を返す（開始前ガード。design.md 8章 I6）。
    DiffLimits limits;
    limits.max_total_bytes = 64;
    DiffEngine engine(limits);
    std::string big(200, 'a'); // 200 バイト > 64
    DiffResult r = engine.compute(big, "small\n");
    EXPECT_TRUE(r.truncated);
    EXPECT_TRUE(r.lines.empty());
    EXPECT_EQ(r.added, 0u);
    EXPECT_EQ(r.removed, 0u);
}

TEST(DiffEngineTest, TooManyLinesTruncates)
{
    DiffLimits limits;
    limits.max_lines = 3;
    DiffEngine engine(limits);
    std::string many = "1\n2\n3\n4\n5\n"; // 5 行 > 3
    DiffResult r = engine.compute("a\n", many);
    EXPECT_TRUE(r.truncated);
}

TEST(DiffEngineTest, OverlongLineTruncates)
{
    DiffLimits limits;
    limits.max_line_bytes = 10;
    DiffEngine engine(limits);
    std::string longline(50, 'z'); // 1 行 50 バイト > 10
    DiffResult r = engine.compute("ok\n", longline + "\n");
    EXPECT_TRUE(r.truncated);
}

TEST(DiffEngineTest, HighEditDistanceTruncatesEvenWithinLineCount)
{
    // 行数・バイト上限内でも、共通行が少なく相違量(編集距離)が大きい入力は開始前に打ち切る
    // （dtl の O(N·D) 暴走防止。両側とも全行相違＝非共通行が max_diff_lines 超）。
    DiffLimits limits;
    limits.max_diff_lines = 5; // テスト用に低く設定
    DiffEngine engine(limits);
    std::string old_c;
    std::string new_c;
    for (int i = 0; i < 20; ++i)
    {
        old_c += "old" + std::to_string(i) + "\n";
        new_c += "new" + std::to_string(i) + "\n";
    }
    DiffResult r = engine.compute(old_c, new_c); // 非共通 old=20 / new=20 > 5
    EXPECT_TRUE(r.truncated);
}

TEST(DiffEngineTest, LargeFileWithScatteredEditsNotTruncatedByDistanceGuard)
{
    // 大ファイルでも共通行が多い（散在する少数編集）なら相違量は小さく、距離ガードで打ち切らない
    // （このツールの本質＝編集レビューを過剰に弾かない。全追加/全削除も同様に通す）。
    DiffLimits limits;
    limits.max_diff_lines = 5;
    DiffEngine engine(limits);
    std::string old_c;
    std::string new_c;
    for (int i = 0; i < 100; ++i)
    {
        const std::string line = "line" + std::to_string(i) + "\n";
        old_c += line;
        new_c += (i == 10 || i == 50) ? ("CHANGED" + std::to_string(i) + "\n") : line;
    }
    DiffResult r = engine.compute(old_c, new_c); // 共通98・非共通 各2 <= 5
    EXPECT_FALSE(r.truncated);
    EXPECT_GT(r.added + r.removed, 0u);
}

TEST(DiffEngineTest, PreCancelledReturnsCancelled)
{
    DiffEngine engine;
    auto token = std::make_shared<CancelToken>();
    token->cancel();
    DiffResult r = engine.compute("a\nb\n", "c\nd\n", token);
    EXPECT_TRUE(r.cancelled);
    EXPECT_FALSE(r.truncated);
}

TEST(DiffEngineTest, NotCancelledWhenTokenClear)
{
    DiffEngine engine;
    auto token = std::make_shared<CancelToken>(); // 未キャンセル
    DiffResult r = engine.compute("a\n", "b\n", token);
    EXPECT_FALSE(r.cancelled);
    EXPECT_EQ(r.removed, 1u);
    EXPECT_EQ(r.added, 1u);
}

TEST(DiffEngineTest, EmptyVsEmptyHasNoChanges)
{
    DiffEngine engine;
    DiffResult r = engine.compute("", "");
    EXPECT_FALSE(r.truncated);
    EXPECT_EQ(r.added, 0u);
    EXPECT_EQ(r.removed, 0u);
    EXPECT_TRUE(r.lines.empty());
}

} // namespace
