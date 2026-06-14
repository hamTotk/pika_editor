// core/render HTML トークナイザの検証（sanitize/inspect の字句解析土台）。
// タグ・終了タグ・属性のクォート種別・自己終了・コメント・生テキスト要素（script/style）の
// 区別が正しいことを観測する（誤った属性化・タグ誤認は XSS 回避の穴になりうる）。
#include "core/render/html_tokenizer.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::render::tokenize_html;
using pika::core::render::TokenType;

TEST(HtmlTokenizerTest, ParsesStartTagWithQuotedAttrs)
{
    auto t = tokenize_html("<a href=\"x.md\" class='c'>");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].type, TokenType::StartTag);
    EXPECT_EQ(t[0].name, "a");
    ASSERT_EQ(t[0].attrs.size(), 2u);
    EXPECT_EQ(t[0].attrs[0].name, "href");
    EXPECT_EQ(t[0].attrs[0].value, "x.md");
    EXPECT_EQ(t[0].attrs[1].name, "class");
    EXPECT_EQ(t[0].attrs[1].value, "c");
}

TEST(HtmlTokenizerTest, ParsesUnquotedAndValuelessAttrs)
{
    auto t = tokenize_html("<input type=text disabled>");
    ASSERT_EQ(t.size(), 1u);
    ASSERT_EQ(t[0].attrs.size(), 2u);
    EXPECT_EQ(t[0].attrs[0].name, "type");
    EXPECT_EQ(t[0].attrs[0].value, "text");
    EXPECT_EQ(t[0].attrs[1].name, "disabled");
    EXPECT_EQ(t[0].attrs[1].value, "");
}

TEST(HtmlTokenizerTest, LowercasesTagAndAttrNames)
{
    auto t = tokenize_html("<IMG SRC=\"a.png\">");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].name, "img");
    ASSERT_EQ(t[0].attrs.size(), 1u);
    EXPECT_EQ(t[0].attrs[0].name, "src");
}

TEST(HtmlTokenizerTest, ParsesSelfClosingTag)
{
    auto t = tokenize_html("<br/>");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].type, TokenType::StartTag);
    EXPECT_TRUE(t[0].self_closing);
}

TEST(HtmlTokenizerTest, ParsesEndTag)
{
    auto t = tokenize_html("</div>");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].type, TokenType::EndTag);
    EXPECT_EQ(t[0].name, "div");
}

TEST(HtmlTokenizerTest, ParsesComment)
{
    auto t = tokenize_html("<!-- hi -->");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].type, TokenType::Comment);
}

TEST(HtmlTokenizerTest, LessThanInTextIsNotTag)
{
    // '3 < 5' の '<' はタグ開始ではなく地のテキスト（タグ名でない文字が続く）。
    auto t = tokenize_html("3 < 5 and 6 > 4");
    // 1 件以上の Text に分解され、StartTag/EndTag を生まない。
    for (const auto& tok : t)
    {
        EXPECT_NE(tok.type, TokenType::StartTag);
        EXPECT_NE(tok.type, TokenType::EndTag);
    }
}

TEST(HtmlTokenizerTest, ScriptBodyIsRawText)
{
    // script の中身（< を含む）を新規タグと誤認せず Text として読み飛ばす。
    auto t = tokenize_html("<script>if (a < b) { x(); }</script>");
    ASSERT_GE(t.size(), 2u);
    EXPECT_EQ(t[0].type, TokenType::StartTag);
    EXPECT_EQ(t[0].name, "script");
    // 中身は 1 つの Text として現れ、'<' 由来の偽タグが生まれない。
    bool saw_inner_text = false;
    for (std::size_t i = 1; i < t.size(); ++i)
    {
        if (t[i].type == TokenType::StartTag)
        {
            FAIL() << "script body must not produce start tags";
        }
        if (t[i].type == TokenType::Text && t[i].text.find("a < b") != std::string::npos)
        {
            saw_inner_text = true;
        }
    }
    EXPECT_TRUE(saw_inner_text);
}

} // namespace
