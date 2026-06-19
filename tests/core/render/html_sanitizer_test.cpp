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
    // 危険 CSS（@import/url()）を含む <style> は丸ごと落とす（style 属性と同じ基準）。
    EXPECT_FALSE(icontains(out, "@import"));
    EXPECT_FALSE(icontains(out, "<style"));
    EXPECT_TRUE(icontains(out, "t"));
}

TEST(HtmlSanitizerTest, KeepsSafeStyleBlock)
{
    // 要件6.3/6.4「インライン CSS を完全レンダリング」。安全な <style> は中身ごと保持する。
    std::string out = sanitize_html(
        "<style>.card{border:1px solid #ccc;box-shadow:0 2px 6px rgba(0,0,0,.1)}</style>"
        "<div class=\"card\">t</div>");
    EXPECT_TRUE(icontains(out, "<style"));
    EXPECT_TRUE(icontains(out, "</style>"));
    EXPECT_TRUE(icontains(out, "box-shadow"));
    EXPECT_TRUE(icontains(out, "border:1px solid"));
    EXPECT_TRUE(icontains(out, "class=\"card\""));
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

// 大文字小文字を保つ厳密包含（実体エスケープの段数を見るため icontains では不十分）。
bool contains_cs(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

// 既に妥当な実体（&amp; / &lt; / &gt; / &quot;）を含むテキストは二重エスケープしない。
// md4c はコード/本文の特殊文字を `&lt;` 等にエスケープして出力するため、サニタイザが '&' を
// さらに `&amp;` 化すると `&amp;lt;` のように二重化し、ブラウザで `&lt;` がリテラル表示される
// （F-004 Mermaid: `-->` → `--&gt;` → 二重化で `--&amp;gt;` → mermaid 構文エラー）。
TEST(HtmlSanitizerTest, DoesNotDoubleEscapeExistingEntities)
{
    // 名前付き標準実体は素通し（1 段のまま）。
    std::string out = sanitize_html("<p>x &amp; y &lt; z &gt; w &quot;q&quot;</p>");
    EXPECT_TRUE(contains_cs(out, "x &amp; y &lt; z &gt; w &quot;q&quot;"));
    EXPECT_FALSE(contains_cs(out, "&amp;amp;"));
    EXPECT_FALSE(contains_cs(out, "&amp;lt;"));
    EXPECT_FALSE(contains_cs(out, "&amp;gt;"));
}

// 数値文字参照（10 進・16 進）も妥当な実体として素通しする。
TEST(HtmlSanitizerTest, PreservesNumericCharacterReferences)
{
    std::string out = sanitize_html("<p>&#169; &#x41; &copy;</p>");
    EXPECT_TRUE(contains_cs(out, "&#169;"));
    EXPECT_TRUE(contains_cs(out, "&#x41;"));
    EXPECT_TRUE(contains_cs(out, "&copy;"));
    EXPECT_FALSE(contains_cs(out, "&amp;#")); // 数値参照の '&' を二重化していない。
}

// 実体になりかけて閉じない／空の '&' は依然エスケープする（素の '&' は安全側で `&amp;`）。
TEST(HtmlSanitizerTest, EscapesBareAmpersand)
{
    // 末尾の裸 '&'、';' で閉じない名前、空の `&;` はいずれも素の '&' 扱い。
    std::string out = sanitize_html("<p>a & b &notclosed &amp c &; d</p>");
    EXPECT_TRUE(contains_cs(out, "a &amp; b"));      // 裸の '&'。
    EXPECT_TRUE(contains_cs(out, "&amp;notclosed")); // ';' 無しは素の '&'。
    EXPECT_TRUE(contains_cs(out, "&amp;amp c"));     // 'amp' の後に ';' が無い（空白）→ 素の '&'。
    EXPECT_TRUE(contains_cs(out, "&amp;; d"));       // 空の名前 `&;` → 素の '&'。
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
