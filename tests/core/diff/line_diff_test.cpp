// core/diff 行差分（dtl/Myers）の検証（sprint6 must）。
// ベースライン内容 vs 現在内容を LF 正規化後に行分割して Myers 差分を計算し、追加/削除/変更行を
// 返すこと、改行のみの差では差分が空になること、色非依存の +/- 記号が必ず付くことを観測する
// （要件8章 / design.md 8章）。
#include "core/diff/line_diff.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using pika::core::diff::diff_lines;
using pika::core::diff::DiffLine;
using pika::core::diff::LineOp;
using pika::core::diff::split_lines_lf;

// テスト補助：op が target の行だけを抽出してその text を集める。
std::vector<std::string> texts_of(const std::vector<DiffLine>& lines, LineOp target)
{
    std::vector<std::string> out;
    for (const auto& l : lines)
    {
        if (l.op == target)
        {
            out.push_back(l.text);
        }
    }
    return out;
}

TEST(SplitLinesLfTest, SplitsOnLfAndDropsTrailingNewline)
{
    auto lines = split_lines_lf("a\nb\nc");
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "a");
    EXPECT_EQ(lines[2], "c");

    auto trailing = split_lines_lf("a\n");
    ASSERT_EQ(trailing.size(), 1u);
    EXPECT_EQ(trailing[0], "a");

    EXPECT_TRUE(split_lines_lf("").empty());
}

TEST(SplitLinesLfTest, NormalizesCrlfToLf)
{
    auto lines = split_lines_lf("a\r\nb\r\n");
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "a"); // CR は落ちる（LF 正規化）
    EXPECT_EQ(lines[1], "b");
}

TEST(LineDiffTest, ReportsAddDeleteChange)
{
    // 行 2 を入れ替え（= delete + add の変更）、行 4 を追加。
    auto base = split_lines_lf("alpha\nbravo\ncharlie\n");
    auto cur = split_lines_lf("alpha\nBRAVO\ncharlie\ndelta\n");
    auto diff = diff_lines(base, cur);

    auto deletes = texts_of(diff, LineOp::Delete);
    auto adds = texts_of(diff, LineOp::Add);

    ASSERT_EQ(deletes.size(), 1u);
    EXPECT_EQ(deletes[0], "bravo");
    // 変更行 BRAVO と新規行 delta の両方が追加側に出る。
    ASSERT_EQ(adds.size(), 2u);
    EXPECT_EQ(adds[0], "BRAVO");
    EXPECT_EQ(adds[1], "delta");
}

TEST(LineDiffTest, CrlfVsLfOnlyDiffIsEmpty)
{
    // 改行コードのみ異なる同一内容は差分が空（要件8章「改行のみの差は出さない」）。
    auto base = split_lines_lf("one\r\ntwo\r\nthree\r\n");
    auto cur = split_lines_lf("one\ntwo\nthree\n");
    auto diff = diff_lines(base, cur);

    EXPECT_TRUE(texts_of(diff, LineOp::Add).empty());
    EXPECT_TRUE(texts_of(diff, LineOp::Delete).empty());
}

TEST(LineDiffTest, MarkerIsColorIndependent)
{
    auto base = split_lines_lf("keep\nold\n");
    auto cur = split_lines_lf("keep\nnew\n");
    auto diff = diff_lines(base, cur);

    // すべての行が +/- / ' ' のいずれかの記号を必ず持つ（色非依存。要件8.4）。
    for (const auto& l : diff)
    {
        const char m = l.marker();
        EXPECT_TRUE(m == '+' || m == '-' || m == ' ');
        if (l.op == LineOp::Add)
        {
            EXPECT_EQ(m, '+');
        }
        if (l.op == LineOp::Delete)
        {
            EXPECT_EQ(m, '-');
        }
        if (l.op == LineOp::Context)
        {
            EXPECT_EQ(m, ' ');
        }
    }
}

TEST(LineDiffTest, UnifiedOrderPlacesDeletesBeforeAdds)
{
    // 変更ブロックでは削除群を先、追加群を後に並べる（unified 表示）。
    auto base = split_lines_lf("x\nold1\nold2\ny\n");
    auto cur = split_lines_lf("x\nnew1\nnew2\ny\n");
    auto diff = diff_lines(base, cur);

    // delete の最後のインデックスが add の最初のインデックスより前にあること。
    std::size_t last_del = 0;
    std::size_t first_add = diff.size();
    for (std::size_t i = 0; i < diff.size(); ++i)
    {
        if (diff[i].op == LineOp::Delete)
        {
            last_del = i;
        }
        if (diff[i].op == LineOp::Add && first_add == diff.size())
        {
            first_add = i;
        }
    }
    EXPECT_LT(last_del, first_add);
}

TEST(LineDiffTest, AllAddedWhenBaselineEmpty)
{
    // 全行追加（新規＝ベースラインなし）の境界。
    auto base = split_lines_lf("");
    auto cur = split_lines_lf("a\nb\n");
    auto diff = diff_lines(base, cur);
    EXPECT_EQ(texts_of(diff, LineOp::Add).size(), 2u);
    EXPECT_TRUE(texts_of(diff, LineOp::Delete).empty());
}

TEST(LineDiffTest, AllDeletedWhenCurrentEmpty)
{
    // 全行削除の境界。
    auto base = split_lines_lf("a\nb\n");
    auto cur = split_lines_lf("");
    auto diff = diff_lines(base, cur);
    EXPECT_EQ(texts_of(diff, LineOp::Delete).size(), 2u);
    EXPECT_TRUE(texts_of(diff, LineOp::Add).empty());
}

TEST(LineDiffTest, IdenticalContentHasNoChanges)
{
    auto base = split_lines_lf("same\ncontent\n");
    auto cur = split_lines_lf("same\ncontent\n");
    auto diff = diff_lines(base, cur);
    EXPECT_TRUE(texts_of(diff, LineOp::Add).empty());
    EXPECT_TRUE(texts_of(diff, LineOp::Delete).empty());
}

} // namespace
