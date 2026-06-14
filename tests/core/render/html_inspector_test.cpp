// core/render HtmlInspector の検証（sprint4 must）。
// <script> タグ・Tailwind CDN 参照を検知してフラグを返すこと、http(s) の画像・CSS・フォント等の
// 外部参照を検知することを観測する（要件6.2・6.3・2.4）。
#include "core/render/html_inspector.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::render::inspect_html;

TEST(HtmlInspectorTest, DetectsScriptTag)
{
    auto r = inspect_html("<html><body><script>alert(1)</script></body></html>");
    EXPECT_TRUE(r.has_script);
    EXPECT_TRUE(r.depends_on_js());
}

TEST(HtmlInspectorTest, NoScriptInPlainHtml)
{
    auto r = inspect_html("<p>just text and <strong>bold</strong></p>");
    EXPECT_FALSE(r.has_script);
    EXPECT_FALSE(r.depends_on_js());
    EXPECT_FALSE(r.has_external_resource);
}

TEST(HtmlInspectorTest, DetectsTailwindCdnViaScriptSrc)
{
    auto r = inspect_html("<script src=\"https://cdn.tailwindcss.com\"></script>");
    EXPECT_TRUE(r.has_tailwind_cdn);
    EXPECT_TRUE(r.depends_on_js());
}

TEST(HtmlInspectorTest, DetectsTailwindCdnViaLinkHref)
{
    auto r = inspect_html(
        "<link href=\"https://cdn.tailwindcss.com/3.0/tailwind.css\" rel=\"stylesheet\">");
    EXPECT_TRUE(r.has_tailwind_cdn);
}

TEST(HtmlInspectorTest, DetectsExternalImage)
{
    auto r = inspect_html("<img src=\"https://example.com/a.png\">");
    EXPECT_TRUE(r.has_external_resource);
}

TEST(HtmlInspectorTest, DetectsExternalCssLink)
{
    auto r = inspect_html("<link rel=\"stylesheet\" href=\"http://cdn.example.com/x.css\">");
    EXPECT_TRUE(r.has_external_resource);
}

TEST(HtmlInspectorTest, DetectsProtocolRelativeExternal)
{
    auto r = inspect_html("<img src=\"//cdn.example.com/a.png\">");
    EXPECT_TRUE(r.has_external_resource);
}

TEST(HtmlInspectorTest, LocalRelativeImageIsNotExternal)
{
    auto r = inspect_html("<img src=\"images/local.png\">");
    EXPECT_FALSE(r.has_external_resource);
}

TEST(HtmlInspectorTest, DetectsExternalFontInStyleBlock)
{
    auto r = inspect_html("<style>@font-face{src:url(https://fonts.example.com/x.woff2);}</style>");
    EXPECT_TRUE(r.has_external_resource);
}

TEST(HtmlInspectorTest, EmptyInputDetectsNothing)
{
    auto r = inspect_html("");
    EXPECT_FALSE(r.has_script);
    EXPECT_FALSE(r.has_tailwind_cdn);
    EXPECT_FALSE(r.has_external_resource);
}

} // namespace
