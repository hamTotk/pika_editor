// core/diff 行内強調（単語/文字単位 LCS）の検証（sprint6 must）。
// 空白区切りトークンの LCS で変更語を特定し、トークン境界が取れない日本語等では文字単位 LCS へ
// フォールバックすることを観測する（要件8.2 / design.md 8章）。
#include "core/diff/inline_diff.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace
{

using pika::core::diff::compute_inline_spans;
using pika::core::diff::InlineSpan;

// 区間群が指す部分文字列を連結して返す（強調された実体を文字列で照合する）。
std::string highlighted(std::string_view line, const std::vector<InlineSpan>& spans)
{
    std::string out;
    for (const auto& s : spans)
    {
        out += std::string(line.substr(s.begin, s.end - s.begin));
    }
    return out;
}

TEST(InlineDiffTest, WordLevelHighlightsChangedWord)
{
    // 空白区切りで「fox」だけが変わる。語単位 LCS で fox/cat が強調される。
    std::string_view old_line = "the quick brown fox";
    std::string_view new_line = "the quick brown cat";
    std::vector<InlineSpan> so;
    std::vector<InlineSpan> sn;
    compute_inline_spans(old_line, new_line, so, sn);

    EXPECT_EQ(highlighted(old_line, so), "fox");
    EXPECT_EQ(highlighted(new_line, sn), "cat");
}

TEST(InlineDiffTest, WordLevelHandlesInsertion)
{
    // 語の挿入。挿入された「very」だけが new 側で強調される（old 側は強調なし）。
    std::string_view old_line = "a b c";
    std::string_view new_line = "a very b c";
    std::vector<InlineSpan> so;
    std::vector<InlineSpan> sn;
    compute_inline_spans(old_line, new_line, so, sn);

    EXPECT_TRUE(highlighted(old_line, so).empty());
    // 強調の実体に "very" を含む（前後の空白がマージされ得るため部分一致で確認）。
    EXPECT_NE(highlighted(new_line, sn).find("very"), std::string::npos);
}

TEST(InlineDiffTest, JapaneseFallsBackToCharLevel)
{
    // 空白を含まない日本語は語境界が取れないため文字単位 LCS にフォールバックする。
    // 「猫」→「犬」の 1 文字だけが変わる（行全体が変更にならない＝強調が機能する）。
    std::string_view old_line = "吾輩は猫である";
    std::string_view new_line = "吾輩は犬である";
    std::vector<InlineSpan> so;
    std::vector<InlineSpan> sn;
    compute_inline_spans(old_line, new_line, so, sn);

    EXPECT_EQ(highlighted(old_line, so), "猫");
    EXPECT_EQ(highlighted(new_line, sn), "犬");
}

TEST(InlineDiffTest, CharFallbackDoesNotSplitMultibyte)
{
    // フォールバック時も UTF-8 コードポイント境界を割らない（区間境界が常に文字頭/末）。
    std::string_view old_line = "あいう";
    std::string_view new_line = "あXう";
    std::vector<InlineSpan> so;
    std::vector<InlineSpan> sn;
    compute_inline_spans(old_line, new_line, so, sn);

    // old 側で「い」（3 バイト）全体が、new 側で「X」（1 バイト）が強調される。
    EXPECT_EQ(highlighted(old_line, so), "い");
    EXPECT_EQ(highlighted(new_line, sn), "X");
    // 区間端が UTF-8 先頭バイト境界に揃っていること（継続バイト 0x80-0xBF で始まらない）。
    for (const auto& s : so)
    {
        if (s.begin < old_line.size())
        {
            const unsigned char b = static_cast<unsigned char>(old_line[s.begin]);
            EXPECT_FALSE((b & 0xC0) == 0x80);
        }
    }
}

TEST(InlineDiffTest, IdenticalLinesHaveNoSpans)
{
    std::string_view line = "no change here";
    std::vector<InlineSpan> so;
    std::vector<InlineSpan> sn;
    compute_inline_spans(line, line, so, sn);
    EXPECT_TRUE(so.empty());
    EXPECT_TRUE(sn.empty());
}

TEST(InlineDiffTest, HugeNoSpaceLineFallsBackWithoutHugeAllocation)
{
    // 空白なしの巨大行（CJK・URL・1 行 JSON 相当）。素朴な文字単位 n×m LCS では DP 行列が
    // ~数十 GB（200K×200K×8B）に達して OOM するため、開始前のセル数ガードでトリム近似へ倒れる。
    // 中央 1 文字だけ差し替え、共通前後が剥がされて中央差分だけが強調されることを観測する。
    std::string old_line(200000, 'a');
    std::string new_line = old_line;
    new_line[100000] = 'b';

    std::vector<InlineSpan> so;
    std::vector<InlineSpan> sn;
    compute_inline_spans(old_line, new_line, so, sn); // OOM せず（ガード）即座に返ること自体が要点

    // トリム近似は共通前後を剥がし、中央の相違 1 文字を 1 区間にする。
    EXPECT_EQ(highlighted(old_line, so), "a");
    EXPECT_EQ(highlighted(new_line, sn), "b");
}

TEST(InlineDiffTest, HugeLineFallbackKeepsUtf8Boundaries)
{
    // 巨大行のトリムフォールバックでも UTF-8 コードポイント境界を割らない（区間端が継続バイトで
    // 始まらない）。共通前後に多バイト文字を含め、中央の差分文字を多バイトにする。
    std::string prefix;
    for (int i = 0; i < 60000; ++i)
    {
        prefix += "あ"; // 3 バイト ×60000 = 180000 バイトの共通前置（空白なし）
    }
    const std::string old_line = prefix + "猫" + prefix;
    const std::string new_line = prefix + "犬" + prefix;

    std::vector<InlineSpan> so;
    std::vector<InlineSpan> sn;
    compute_inline_spans(old_line, new_line, so, sn);

    EXPECT_EQ(highlighted(old_line, so), "猫");
    EXPECT_EQ(highlighted(new_line, sn), "犬");
    for (const auto& s : so)
    {
        if (s.begin < old_line.size())
        {
            const unsigned char b = static_cast<unsigned char>(old_line[s.begin]);
            EXPECT_FALSE((b & 0xC0) == 0x80); // 区間頭が継続バイトでない＝文字を割っていない
        }
    }
}

TEST(InlineDiffTest, MixedAsciiAndJapaneseUsesCharFallbackWhenNoSpaces)
{
    // 空白の無い ASCII+日本語混在も文字単位フォールバック。末尾の差分のみ強調される。
    std::string_view old_line = "id=猫123";
    std::string_view new_line = "id=猫456";
    std::vector<InlineSpan> so;
    std::vector<InlineSpan> sn;
    compute_inline_spans(old_line, new_line, so, sn);

    EXPECT_EQ(highlighted(old_line, so), "123");
    EXPECT_EQ(highlighted(new_line, sn), "456");
}

} // namespace
