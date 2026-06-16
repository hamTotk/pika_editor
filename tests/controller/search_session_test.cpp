// controller/search_session の検証（sprint7 must#3）。
// - SearchEngine（find_all/replace_all・PCRE2・キャンセル）を呼ぶ結線（Result/truncated/cancelled
//   をそのまま透過する）。
// - 検索結果のカーソル遷移（次へ/前へ・折り返し）の決定論。
// - 置換適用（リテラル/正規表現キャプチャ参照）の決定論。
#include "controller/search_session.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>

namespace
{

using pika::controller::NavTarget;
using pika::controller::next_match;
using pika::controller::prev_match;
using pika::controller::SearchSession;
using pika::core::search::CancelToken;
using pika::core::search::SearchOptions;
using pika::core::search::SearchResult;

// ---- find_all 結線（SearchEngine へ委譲・件数を観測） ----

TEST(SearchSessionTest, FindAllReturnsAllHits)
{
    SearchSession s;
    SearchOptions opts; // 既定: 大文字小文字区別・リテラル
    const auto r = s.find_all("ab ab ab", "ab", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().count(), 3u);
}

TEST(SearchSessionTest, FindAllRegexHits)
{
    SearchSession s;
    SearchOptions opts;
    opts.regex = true;
    const auto r = s.find_all("a1 b2 c3", "[a-z][0-9]", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().count(), 3u);
}

TEST(SearchSessionTest, FindAllInvalidRegexIsError)
{
    SearchSession s;
    SearchOptions opts;
    opts.regex = true;
    const auto r = s.find_all("text", "(unclosed", opts);
    // 不正な正規表現は SearchEngine が InvalidArgument を返す（握り潰さず透過）。
    EXPECT_TRUE(r.is_err());
}

TEST(SearchSessionTest, FindAllCancelledTransparent)
{
    SearchSession s;
    SearchOptions opts;
    auto token = std::make_shared<CancelToken>();
    token->cancel(); // 開始前にキャンセル済み
    const auto r = s.find_all("ab ab ab", "ab", opts, token);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().cancelled); // cancelled をそのまま透過
}

// ---- replace_all 結線 ----

TEST(SearchSessionTest, ReplaceAllLiteral)
{
    SearchSession s;
    SearchOptions opts;
    const auto r = s.replace_all("foo foo", "foo", "bar", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().text, "bar bar");
    EXPECT_EQ(r.value().replaced, 2u);
}

TEST(SearchSessionTest, ReplaceAllRegexCaptureReference)
{
    SearchSession s;
    SearchOptions opts;
    opts.regex = true;
    // キャプチャ参照 $1 の展開（要件5.4）。
    const auto r = s.replace_all("a=1 b=2", "([a-z])=([0-9])", "$2:$1", opts);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().text, "1:a 2:b");
    EXPECT_EQ(r.value().replaced, 2u);
}

// ---- カーソル遷移：next_match ----

// caret から次のヒットを選ぶための SearchResult を組み立てる補助（begin 昇順）。
SearchResult make_result(std::initializer_list<std::pair<std::size_t, std::size_t>> ranges)
{
    SearchResult r;
    for (const auto& [b, e] : ranges)
    {
        pika::core::search::Match m;
        m.begin = b;
        m.end = e;
        r.matches.push_back(m);
    }
    return r;
}

TEST(SearchSessionTest, NextMatchFromBeforeFirst)
{
    const SearchResult r = make_result({{2, 4}, {10, 12}, {20, 22}});
    const NavTarget t = next_match(r, 0);
    EXPECT_TRUE(t.found);
    EXPECT_EQ(t.begin, 2u);
    EXPECT_EQ(t.index, 0u);
    EXPECT_EQ(t.total, 3u);
    EXPECT_FALSE(t.wrapped);
}

TEST(SearchSessionTest, NextMatchSkipsHitAtCaret)
{
    // caret がちょうどヒット先頭にあるときは「次」へ進む（同位置に留まらない）。
    const SearchResult r = make_result({{2, 4}, {10, 12}});
    const NavTarget t = next_match(r, 2);
    EXPECT_TRUE(t.found);
    EXPECT_EQ(t.begin, 10u);
    EXPECT_EQ(t.index, 1u);
    EXPECT_FALSE(t.wrapped);
}

TEST(SearchSessionTest, NextMatchWrapsToFirst)
{
    const SearchResult r = make_result({{2, 4}, {10, 12}});
    const NavTarget t = next_match(r, 100); // 末尾より後 → 先頭へ折り返す
    EXPECT_TRUE(t.found);
    EXPECT_EQ(t.begin, 2u);
    EXPECT_EQ(t.index, 0u);
    EXPECT_TRUE(t.wrapped);
}

TEST(SearchSessionTest, NextMatchEmptyNotFound)
{
    const SearchResult r = make_result({});
    const NavTarget t = next_match(r, 0);
    EXPECT_FALSE(t.found);
    EXPECT_EQ(t.total, 0u);
}

// ---- カーソル遷移：prev_match ----

TEST(SearchSessionTest, PrevMatchFromAfterLast)
{
    const SearchResult r = make_result({{2, 4}, {10, 12}, {20, 22}});
    const NavTarget t = prev_match(r, 100);
    EXPECT_TRUE(t.found);
    EXPECT_EQ(t.begin, 20u);
    EXPECT_EQ(t.index, 2u);
    EXPECT_FALSE(t.wrapped);
}

TEST(SearchSessionTest, PrevMatchSkipsHitAtCaret)
{
    const SearchResult r = make_result({{2, 4}, {10, 12}});
    const NavTarget t = prev_match(r, 10); // begin<10 の最後＝{2,4}
    EXPECT_TRUE(t.found);
    EXPECT_EQ(t.begin, 2u);
    EXPECT_EQ(t.index, 0u);
    EXPECT_FALSE(t.wrapped);
}

TEST(SearchSessionTest, PrevMatchWrapsToLast)
{
    const SearchResult r = make_result({{2, 4}, {10, 12}});
    const NavTarget t = prev_match(r, 0); // 先頭より前 → 末尾へ折り返す
    EXPECT_TRUE(t.found);
    EXPECT_EQ(t.begin, 10u);
    EXPECT_EQ(t.index, 1u);
    EXPECT_TRUE(t.wrapped);
}

TEST(SearchSessionTest, PrevMatchEmptyNotFound)
{
    const SearchResult r = make_result({});
    const NavTarget t = prev_match(r, 50);
    EXPECT_FALSE(t.found);
}

// ---- 遷移の決定論（同一入力で同一出力） ----

TEST(SearchSessionTest, NavigationDeterministic)
{
    const SearchResult r = make_result({{2, 4}, {10, 12}, {20, 22}});
    const NavTarget a = next_match(r, 5);
    const NavTarget b = next_match(r, 5);
    EXPECT_EQ(a.begin, b.begin);
    EXPECT_EQ(a.index, b.index);
    EXPECT_EQ(a.wrapped, b.wrapped);
}

} // namespace
