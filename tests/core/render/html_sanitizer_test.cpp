// core/render ホワイトリストサニタイズの検証（sprint4 must）。
// 許可タグ・許可属性のみ通し、<script>・イベント属性(onload等)・javascript: URL・
// <iframe>/<object>/<embed>/<base> を除去すること、インライン SVG の script/foreignObject/
// イベント属性を除去すること、CSS の url()/@import を遮断することを観測する（design.md 6章）。
#include "core/render/html_sanitizer.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::render::sanitize_html;

// 出力に部分文字列が含まれるか（大文字小文字を無視）。
bool icontains(const std::string& hay, const std::string& needle)
{
    auto lower = [](std::string s) {
        for (char& c : s)
        {
            if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return s;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

TEST(HtmlSanitizerTest, AllowsBasicMarkdownTags)
{
    std::string out = sanitize_html("<p>hello <strong>world</strong></p>");
    EXPECT_EQ(out, "<p>hello <strong>world</strong></p>");
}

TEST(HtmlSanitizerTest, RemovesScriptTagAndContent)
{
    std::string out = sanitize_html("<p>ok</p><script>alert(1)</script><p>after</p>");
    // script タグと中身（alert(1)）が消え、前後の段落は残る。
    EXPECT_FALSE(icontains(out, "<script"));
    EXPECT_FALSE(icontains(out, "alert(1)"));
    EXPECT_TRUE(icontains(out, "ok"));
    EXPECT_TRUE(icontains(out, "after"));
}

TEST(HtmlSanitizerTest, RemovesEventHandlerAttributes)
{
    std::string out = sanitize_html("<img src=\"a.png\" onload=\"steal()\" onerror=\"x()\">");
    EXPECT_FALSE(icontains(out, "onload"));
    EXPECT_FALSE(icontains(out, "onerror"));
    EXPECT_FALSE(icontains(out, "steal"));
    // 安全な src は残る。
    EXPECT_TRUE(icontains(out, "a.png"));
}

TEST(HtmlSanitizerTest, RemovesJavascriptUrlInHref)
{
    std::string out = sanitize_html("<a href=\"javascript:alert(1)\">click</a>");
    EXPECT_FALSE(icontains(out, "javascript:"));
    // タグ自体（a）とテキストは残るが href は落ちる。
    EXPECT_TRUE(icontains(out, "click"));
}

TEST(HtmlSanitizerTest, RemovesObfuscatedJavascriptUrl)
{
    // エンティティ・大文字・前後空白・タブによる難読化を正規化して除去する。
    std::string out = sanitize_html("<a href=\"  JaVa&#115;cript:alert(1)\">x</a>");
    EXPECT_FALSE(icontains(out, "alert(1)"));
}

TEST(HtmlSanitizerTest, RemovesIframeObjectEmbedBase)
{
    std::string out = sanitize_html("<iframe src=\"evil\"></iframe><object data=\"x\"></object>"
                                    "<embed src=\"y\"><base href=\"http://evil\">");
    EXPECT_FALSE(icontains(out, "<iframe"));
    EXPECT_FALSE(icontains(out, "<object"));
    EXPECT_FALSE(icontains(out, "<embed"));
    EXPECT_FALSE(icontains(out, "<base"));
}

TEST(HtmlSanitizerTest, NeutralizesInlineSvgScriptAndForeignObject)
{
    std::string out = sanitize_html("<svg><script>alert(1)</script>"
                                    "<foreignObject><div onclick=\"x()\">y</div></foreignObject>"
                                    "<circle cx=\"5\" cy=\"5\" r=\"3\"></circle></svg>");
    EXPECT_FALSE(icontains(out, "<script"));
    EXPECT_FALSE(icontains(out, "alert(1)"));
    EXPECT_FALSE(icontains(out, "foreignobject"));
    EXPECT_FALSE(icontains(out, "onclick"));
    // 安全な描画要素（circle）は残る。
    EXPECT_TRUE(icontains(out, "<circle"));
}

TEST(HtmlSanitizerTest, RemovesEventAttributeInsideSvg)
{
    std::string out = sanitize_html("<svg onload=\"alert(1)\"><rect onclick=\"x()\"/></svg>");
    EXPECT_FALSE(icontains(out, "onload"));
    EXPECT_FALSE(icontains(out, "onclick"));
    EXPECT_TRUE(icontains(out, "<svg"));
}

TEST(HtmlSanitizerTest, BlocksCssUrlInStyleAttribute)
{
    std::string out = sanitize_html("<div style=\"background:url(http://evil/x.png)\">t</div>");
    // url() を含む style は丸ごと落とす。
    EXPECT_FALSE(icontains(out, "url("));
    EXPECT_FALSE(icontains(out, "style="));
    // div とテキストは残る。
    EXPECT_TRUE(icontains(out, ">t<"));
}

TEST(HtmlSanitizerTest, BlocksCssImport)
{
    std::string out = sanitize_html("<style>@import url(http://evil/a.css);</style><p>t</p>");
    // <style> は丸ごとサブツリー除去されるため @import は出力されない。
    EXPECT_FALSE(icontains(out, "@import"));
    EXPECT_FALSE(icontains(out, "<style"));
    EXPECT_TRUE(icontains(out, "t"));
}

TEST(HtmlSanitizerTest, KeepsSafeStyleAttribute)
{
    std::string out = sanitize_html("<span style=\"color:red\">t</span>");
    EXPECT_TRUE(icontains(out, "color:red"));
}

TEST(HtmlSanitizerTest, EscapesTextContent)
{
    // 未許可タグ（custom-x）はタグを落として中身を残すが、テキストはエスケープされる。
    std::string out = sanitize_html("<custom-x>a &amp; b < c</custom-x>");
    EXPECT_FALSE(icontains(out, "<custom-x"));
    // '<' は実体参照に。スクリプト挿入の余地を残さない。
    EXPECT_TRUE(icontains(out, "&lt;"));
}

TEST(HtmlSanitizerTest, RemovesHtmlComments)
{
    std::string out = sanitize_html("<p>a</p><!-- [if IE]><script>x()</script><![endif] -->");
    EXPECT_FALSE(icontains(out, "<!--"));
    EXPECT_FALSE(icontains(out, "x()"));
}

TEST(HtmlSanitizerTest, KeepsGfmTableTags)
{
    std::string out = sanitize_html("<table><thead><tr><th>h</th></tr></thead>"
                                    "<tbody><tr><td>d</td></tr></tbody></table>");
    EXPECT_TRUE(icontains(out, "<table>"));
    EXPECT_TRUE(icontains(out, "<th>"));
    EXPECT_TRUE(icontains(out, "<td>"));
}

TEST(HtmlSanitizerTest, KeepsSafeRelativeImage)
{
    std::string out = sanitize_html("<img src=\"images/a.png\" alt=\"pic\">");
    EXPECT_TRUE(icontains(out, "images/a.png"));
    EXPECT_TRUE(icontains(out, "alt=\"pic\""));
}

TEST(HtmlSanitizerTest, EmptyInputYieldsEmptyOutput)
{
    EXPECT_EQ(sanitize_html(""), "");
}

} // namespace
