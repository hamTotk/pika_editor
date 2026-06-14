// core/search SearchEngine（PCRE2=pcre2-16/UTF）の検証。sprint9 must/should。
// 検索（大小区別・単語単位・正規表現・全ヒット列挙・件数）、正規表現置換（キャプチャ参照=後方参照）、
// Unicode文字クラス、巨大入力ガード、協調キャンセルを観測する（要件5.4 / design.md 3章
// core/search）。
#include "core/search/cancel_token.h"
#include "core/search/search_engine.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using pika::core::search::CancelToken;
using pika::core::search::ReplaceResult;
using pika::core::search::SearchEngine;
using pika::core::search::SearchLimits;
using pika::core::search::SearchOptions;
using pika::core::search::SearchResult;
using pika::util::ErrorCode;

// ---- 検索（find_all）----

TEST(SearchEngineTest, LiteralFindsAllHitsAndCount)
{
    SearchEngine engine;
    SearchOptions opts; // 既定: 大小区別あり・単語単位なし・リテラル
    auto r = engine.find_all("ababab", "ab", opts);
    ASSERT_TRUE(r.is_ok());
    const SearchResult& sr = r.value();
    EXPECT_FALSE(sr.truncated);
    EXPECT_FALSE(sr.cancelled);
    EXPECT_EQ(sr.count(), 3u);
    EXPECT_EQ(sr.matches[0].begin, 0u);
    EXPECT_EQ(sr.matches[0].end, 2u);
    EXPECT_EQ(sr.matches[1].begin, 2u);
    EXPECT_EQ(sr.matches[2].begin, 4u);
}

TEST(SearchEngineTest, CaseSensitiveDistinguishesCase)
{
    SearchEngine engine;
    SearchOptions opts;
    opts.case_sensitive = true;
    auto r = engine.find_all("Foo foo FOO", "foo", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().count(), 1u); // 小文字 "foo" のみ
}

TEST(SearchEngineTest, CaseInsensitiveMatchesAllCases)
{
    SearchEngine engine;
    SearchOptions opts;
    opts.case_sensitive = false;
    auto r = engine.find_all("Foo foo FOO", "foo", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().count(), 3u);
}

TEST(SearchEngineTest, WholeWordDoesNotMatchSubstring)
{
    SearchEngine engine;
    SearchOptions opts;
    opts.whole_word = true;
    // "cat" は "category" の一部にはマッチせず、独立した "cat" にのみマッチする。
    auto r = engine.find_all("cat category cat", "cat", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().count(), 2u);
}

TEST(SearchEngineTest, RegexMatchesPattern)
{
    SearchEngine engine;
    SearchOptions opts;
    opts.regex = true;
    auto r = engine.find_all("a1 b22 c333", "[a-z][0-9]+", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().count(), 3u);
}

TEST(SearchEngineTest, LiteralTreatsMetacharsLiterally)
{
    SearchEngine engine;
    SearchOptions opts; // regex=false
    // リテラルでは "a.c" はドットをメタ文字扱いせず、文字列 "a.c" のみにマッチする。
    auto r = engine.find_all("a.c abc axc", "a.c", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().count(), 1u);
    EXPECT_EQ(r.value().matches[0].begin, 0u);
}

TEST(SearchEngineTest, InvalidRegexReturnsErrorNotException)
{
    SearchEngine engine;
    SearchOptions opts;
    opts.regex = true;
    auto r = engine.find_all("text", "(unterminated", opts);
    ASSERT_TRUE(r.is_err()); // 例外を投げず Result::err
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
}

TEST(SearchEngineTest, EmptyPatternYieldsNoHits)
{
    SearchEngine engine;
    SearchOptions opts;
    auto r = engine.find_all("abc", "", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().count(), 0u);
}

TEST(SearchEngineTest, CapturesGroupsInMatch)
{
    SearchEngine engine;
    SearchOptions opts;
    opts.regex = true;
    auto r = engine.find_all("key=val", "(\\w+)=(\\w+)", opts);
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().count(), 1u);
    const auto& m = r.value().matches[0];
    ASSERT_GE(m.groups.size(), 3u); // [0]=全体, [1], [2]
    // group[1] = "key"（バイト範囲 0..3）, group[2] = "val"（4..7）
    EXPECT_TRUE(m.groups[1].matched);
    EXPECT_EQ(m.groups[1].begin, 0u);
    EXPECT_EQ(m.groups[1].end, 3u);
    EXPECT_TRUE(m.groups[2].matched);
    EXPECT_EQ(m.groups[2].begin, 4u);
    EXPECT_EQ(m.groups[2].end, 7u);
}

// ---- 正規表現置換（replace_all・キャプチャ参照=後方参照）----

TEST(SearchEngineTest, RegexReplaceWithCaptureReferenceDollar)
{
    SearchEngine engine;
    SearchOptions opts;
    opts.regex = true;
    // (\w+)=(\w+) を $2=$1
    // に入れ替える（キャプチャ参照。要件5.4「正規表現＋キャプチャ参照で全置換」）。
    auto r = engine.replace_all("a=1 b=2", "(\\w+)=(\\w+)", "$2=$1", opts);
    ASSERT_TRUE(r.is_ok());
    const ReplaceResult& rr = r.value();
    EXPECT_EQ(rr.replaced, 2u);
    EXPECT_EQ(rr.text, "1=a 2=b");
}

TEST(SearchEngineTest, RegexReplaceWithBackslashReference)
{
    SearchEngine engine;
    SearchOptions opts;
    opts.regex = true;
    // \1 形式の後方参照も受け付ける。
    auto r = engine.replace_all("John Smith", "(\\w+) (\\w+)", "\\2 \\1", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().text, "Smith John");
    EXPECT_EQ(r.value().replaced, 1u);
}

TEST(SearchEngineTest, RegexReplaceBracedGroupAndDollarLiteral)
{
    SearchEngine engine;
    SearchOptions opts;
    opts.regex = true;
    // ${1} の波括弧形式と $$（リテラルの '$'）。
    auto r = engine.replace_all("price 10", "(\\d+)", "$$${1}.00", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().text, "price $10.00");
}

TEST(SearchEngineTest, LiteralReplaceDoesNotExpandReferences)
{
    SearchEngine engine;
    SearchOptions opts; // regex=false
    // リテラル置換はキャプチャ参照を展開しない（"$1" はそのまま）。
    auto r = engine.replace_all("aaa", "a", "$1", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().text, "$1$1$1");
    EXPECT_EQ(r.value().replaced, 3u);
}

TEST(SearchEngineTest, ReplaceAllPreservesUnmatchedText)
{
    SearchEngine engine;
    SearchOptions opts;
    auto r = engine.replace_all("xx-yy-zz", "-", "_", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().text, "xx_yy_zz");
    EXPECT_EQ(r.value().replaced, 2u);
}

TEST(SearchEngineTest, ReplaceNoMatchReturnsOriginal)
{
    SearchEngine engine;
    SearchOptions opts;
    auto r = engine.replace_all("hello", "zzz", "x", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().text, "hello");
    EXPECT_EQ(r.value().replaced, 0u);
}

// ---- Unicode文字クラス（pcre2-16/UTF + UCP）----

TEST(SearchEngineTest, UnicodePropertyMatchesNonAscii)
{
    SearchEngine engine;
    SearchOptions opts;
    opts.regex = true;
    // \p{L}+（Unicode の「文字」プロパティ）が日本語にマッチする（UTF + UCP。要件5.4）。
    // "日本語" は UTF-8 で 9 バイト（各 3 バイト）。マッチは 0..9。
    auto r = engine.find_all("日本語", "\\p{L}+", opts);
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().count(), 1u);
    EXPECT_EQ(r.value().matches[0].begin, 0u);
    EXPECT_EQ(r.value().matches[0].end, 9u);
}

TEST(SearchEngineTest, UnicodeWordClassIncludesNonAscii)
{
    SearchEngine engine;
    SearchOptions opts;
    opts.regex = true;
    // UCP 下で \w が日本語を含む（ASCII 限定にならない）。"あ" は 3 バイト。
    auto r = engine.find_all("xあ", "\\w+", opts);
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().count(), 1u);
    EXPECT_EQ(r.value().matches[0].begin, 0u);
    EXPECT_EQ(r.value().matches[0].end, 4u); // 'x'(1) + 'あ'(3)
}

TEST(SearchEngineTest, NonAsciiByteOffsetsAreUtf8)
{
    SearchEngine engine;
    SearchOptions opts;
    // 非 ASCII を挟んだ位置のバイトオフセットが UTF-8 基準で正しく写し戻る。
    // "あ" (3B) + "b" → "b" のオフセットは 3。
    auto r = engine.find_all("あb", "b", opts);
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().count(), 1u);
    EXPECT_EQ(r.value().matches[0].begin, 3u);
    EXPECT_EQ(r.value().matches[0].end, 4u);
}

TEST(SearchEngineTest, SurrogatePairOffsetsMapToUtf8)
{
    SearchEngine engine;
    SearchOptions opts;
    // BMP外（絵文字 U+1F600=4バイト UTF-8 / サロゲートペア UTF-16）を挟んでも u8
    // オフセットが正しい。
    std::string text = "\xF0\x9F\x98\x80z"; // 😀 + 'z'
    auto r = engine.find_all(text, "z", opts);
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().count(), 1u);
    EXPECT_EQ(r.value().matches[0].begin, 4u);
    EXPECT_EQ(r.value().matches[0].end, 5u);
}

// ---- 巨大入力ガード ----

TEST(SearchEngineTest, OversizeInputTruncatesBeforeSearching)
{
    SearchLimits limits;
    limits.max_total_bytes = 8;
    SearchEngine engine(limits);
    SearchOptions opts;
    auto r = engine.find_all("0123456789", "1", opts); // 10 バイト > 8
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().truncated);
    EXPECT_EQ(r.value().count(), 0u);
}

TEST(SearchEngineTest, OversizeInputTruncatesReplace)
{
    SearchLimits limits;
    limits.max_total_bytes = 4;
    SearchEngine engine(limits);
    SearchOptions opts;
    auto r = engine.replace_all("abcdef", "a", "X", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().truncated);
    EXPECT_EQ(r.value().replaced, 0u);
}

TEST(SearchEngineTest, MaxMatchesCapTruncates)
{
    SearchLimits limits;
    limits.max_matches = 2;
    SearchEngine engine(limits);
    SearchOptions opts;
    auto r = engine.find_all("aaaaa", "a", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().truncated);
    EXPECT_EQ(r.value().count(), 2u);
}

// ---- 自己 ReDoS（破滅的バックトラック）の境界 ----

TEST(SearchEngineTest, CatastrophicBacktrackingIsBoundedNotHang)
{
    // ユーザー入力の正規表現は `(a+)+$` のように指数的バックトラックを誘発し得る。match/depth limit
    // を 設定したマッチコンテキストにより、ハングせず有限ステップで打ち切られ truncated
    // を立てて返る ことを観測する（sprint9 high）。limit を低めにして打ち切りを決定論化する。
    SearchLimits limits;
    limits.match_limit = 1000;
    limits.depth_limit = 100;
    SearchEngine engine(limits);
    SearchOptions opts;
    opts.regex = true;

    // 末尾に非マッチ要素（X）があり全体が $ に達せない →
    // 先頭位置で破滅的バックトラックが起きる入力。
    std::string subject(60, 'a');
    subject += 'X';

    auto r = engine.find_all(subject, "(a+)+$", opts);
    // 返ること自体（無限ループしない）が要点。上限到達は truncated で通知される。
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().truncated);
}

TEST(SearchEngineTest, NormalRegexNotAffectedByLimits)
{
    // 通常の正規表現は既定 limit 内で完了し truncated にならない（上限導入の副作用がないこと）。
    SearchEngine engine;
    SearchOptions opts;
    opts.regex = true;
    auto r = engine.find_all("abc123def456", "[0-9]+", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value().truncated);
    EXPECT_EQ(r.value().count(), 2u);
}

TEST(SearchEngineTest, ComplexityLimitSetsTruncateReason)
{
    // 自己 ReDoS 打ち切りは truncate_reason=ComplexityLimit
    // で「入力過大」「件数上限」と区別される。
    SearchLimits limits;
    limits.match_limit = 1000;
    limits.depth_limit = 100;
    SearchEngine engine(limits);
    SearchOptions opts;
    opts.regex = true;
    std::string subject(60, 'a');
    subject += 'X';
    auto r = engine.find_all(subject, "(a+)+$", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().truncated);
    EXPECT_EQ(r.value().truncate_reason, pika::core::search::TruncateReason::ComplexityLimit);
}

TEST(SearchEngineTest, ZeroWidthMatchAfterSupplementaryCharNotDropped)
{
    // ゼロ幅マッチ（\b）が BMP外文字（😀=U+1F600=4バイト/UTF-16サロゲートペア）の前後で正しく
    // 列挙され、以降を取りこぼさない。修正前はゼロ幅マッチの +1 前進が下位サロゲートの途中に着地し
    // pcre2_match が PCRE2_ERROR_BADUTFOFFSET を返し、それを break で握り潰して 😀
    // 以降を欠落させた。
    SearchEngine engine;
    SearchOptions opts;
    opts.regex = true;
    // "a😀b": \b は a前(0)・a後=😀前(1)・😀後=b前(5)・b後=末尾(6) に立つ。
    const std::string text = std::string("a") + "\xF0\x9F\x98\x80" + "b";
    auto r = engine.find_all(text, "\\b", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value().truncated); // BADUTFOFFSET で途中打ち切りにならない
    bool found5 = false;
    bool found6 = false;
    for (const auto& m : r.value().matches)
    {
        if (m.begin == 5u)
        {
            found5 = true; // 😀 の後（b 前）の境界
        }
        if (m.begin == 6u)
        {
            found6 = true; // b の後（末尾）の境界
        }
    }
    EXPECT_TRUE(found5);
    EXPECT_TRUE(found6);
}

// ---- 協調キャンセル ----

TEST(SearchEngineTest, PreCancelledFindReturnsCancelled)
{
    SearchEngine engine;
    SearchOptions opts;
    auto token = std::make_shared<CancelToken>();
    token->cancel();
    auto r = engine.find_all("abcabc", "a", opts, token);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().cancelled);
}

TEST(SearchEngineTest, PreCancelledReplaceReturnsCancelled)
{
    SearchEngine engine;
    SearchOptions opts;
    auto token = std::make_shared<CancelToken>();
    token->cancel();
    auto r = engine.replace_all("abcabc", "a", "X", opts, token);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().cancelled);
    EXPECT_EQ(r.value().replaced, 0u);
}

TEST(SearchEngineTest, CancelDuringIterationStopsAndFlags)
{
    // 各ヒット反復でキャンセルを観測する。最初の 1 件処理後にセットされたフラグで中断する想定。
    // ここでは事前にセットせず、find_all が反復ループ先頭でフラグを見ることを利用し、
    // 「セット済みなら中断」を保証する（協調キャンセル＝別スレッド中断に頼らない）。
    SearchEngine engine;
    SearchOptions opts;
    auto token = std::make_shared<CancelToken>();
    // 多数ヒットを用意し、開始前にキャンセルしておけば 1 件目処理前に中断される。
    std::string text(1000, 'a');
    token->cancel();
    auto r = engine.find_all(text, "a", opts, token);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().cancelled);
    EXPECT_LT(r.value().count(), 1000u); // 全件は列挙しない
}

TEST(SearchEngineTest, NotCancelledWhenTokenClear)
{
    SearchEngine engine;
    SearchOptions opts;
    auto token = std::make_shared<CancelToken>(); // 未キャンセル
    auto r = engine.find_all("aa", "a", opts, token);
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value().cancelled);
    EXPECT_EQ(r.value().count(), 2u);
}

} // namespace
