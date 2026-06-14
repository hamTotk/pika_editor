// core/render URL 分類の検証（sprint4 must の基盤）。
// javascript: URL の難読化（エンティティ・空白・タブ・大文字小文字）正規化と危険判定、
// http(s)・プロトコル相対の外部判定を観測する（design.md 6章）。
#include "core/render/url_classifier.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::render::is_dangerous_url;
using pika::core::render::is_external_url;

TEST(UrlClassifierTest, PlainJavascriptIsDangerous)
{
    EXPECT_TRUE(is_dangerous_url("javascript:alert(1)"));
}

TEST(UrlClassifierTest, MixedCaseJavascriptIsDangerous)
{
    EXPECT_TRUE(is_dangerous_url("JaVaScRiPt:alert(1)"));
}

TEST(UrlClassifierTest, LeadingWhitespaceJavascriptIsDangerous)
{
    EXPECT_TRUE(is_dangerous_url("   \t\n javascript:alert(1)"));
}

TEST(UrlClassifierTest, NumericEntityJavascriptIsDangerous)
{
    // java&#115;cript: → javascript:
    EXPECT_TRUE(is_dangerous_url("java&#115;cript:alert(1)"));
}

TEST(UrlClassifierTest, HexEntityJavascriptIsDangerous)
{
    // &#x6a; == 'j'
    EXPECT_TRUE(is_dangerous_url("&#x6a;avascript:alert(1)"));
}

TEST(UrlClassifierTest, InnerWhitespaceJavascriptIsDangerous)
{
    // スキーム内のタブ・改行による分割を潰す。
    EXPECT_TRUE(is_dangerous_url("java\tscript:alert(1)"));
}

TEST(UrlClassifierTest, VbscriptIsDangerous)
{
    EXPECT_TRUE(is_dangerous_url("vbscript:msgbox(1)"));
}

TEST(UrlClassifierTest, DataUrlIsDangerous)
{
    // ユーザー入力の data: は無条件で危険扱い（pika 生成プレースホルダは別経路）。
    EXPECT_TRUE(is_dangerous_url("data:text/html,<script>alert(1)</script>"));
}

TEST(UrlClassifierTest, RelativeUrlIsNotDangerous)
{
    EXPECT_FALSE(is_dangerous_url("images/a.png"));
    EXPECT_FALSE(is_dangerous_url("./doc.md"));
    EXPECT_FALSE(is_dangerous_url("../up.md"));
}

TEST(UrlClassifierTest, HttpHttpsIsNotDangerousButExternal)
{
    // http(s) は危険スキームではない（外部判定で扱う）。
    EXPECT_FALSE(is_dangerous_url("https://example.com/a.png"));
    EXPECT_TRUE(is_external_url("https://example.com/a.png"));
    EXPECT_TRUE(is_external_url("http://example.com/a.png"));
}

TEST(UrlClassifierTest, ProtocolRelativeIsExternal)
{
    EXPECT_TRUE(is_external_url("//cdn.example.com/a.png"));
}

TEST(UrlClassifierTest, RelativeIsNotExternal)
{
    EXPECT_FALSE(is_external_url("images/a.png"));
    EXPECT_FALSE(is_external_url("/abs/local.png"));
}

} // namespace
