// core/render Markdown→HTML 変換の検証（sprint4 should）。
// md4c による GFM 変換（テーブル・タスクリスト・打消し線・自動リンク）が呼び出せること、
// 出力が必ずサニタイズされること（Markdown 内 raw HTML の script が除去される）を観測する。
#include "core/render/markdown_renderer.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::render::render_markdown;

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

TEST(MarkdownRendererTest, RendersHeadingAndParagraph)
{
    auto r = render_markdown("# Title\n\nHello **world**.\n");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(contains(r.value(), "<h1>"));
    EXPECT_TRUE(contains(r.value(), "<strong>world</strong>"));
}

TEST(MarkdownRendererTest, RendersGfmTable)
{
    auto r = render_markdown("| a | b |\n|---|---|\n| 1 | 2 |\n");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(contains(r.value(), "<table>"));
    EXPECT_TRUE(contains(r.value(), "<td>1</td>"));
}

TEST(MarkdownRendererTest, RendersStrikethrough)
{
    auto r = render_markdown("~~gone~~\n");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(contains(r.value(), "<del>gone</del>"));
}

TEST(MarkdownRendererTest, RendersTaskList)
{
    auto r = render_markdown("- [x] done\n- [ ] todo\n");
    ASSERT_TRUE(r.is_ok());
    // タスクリストはチェックボックス input を生成する。
    EXPECT_TRUE(contains(r.value(), "type=\"checkbox\""));
}

TEST(MarkdownRendererTest, RendersAutolink)
{
    auto r = render_markdown("see https://example.com here\n");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(contains(r.value(), "href=\"https://example.com\""));
}

TEST(MarkdownRendererTest, SanitizesRawHtmlScript)
{
    // Markdown 内 raw HTML の <script> は出力されない（変換後に必ずサニタイズ）。
    auto r = render_markdown("text\n\n<script>alert(1)</script>\n\nmore\n");
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(contains(r.value(), "<script"));
    EXPECT_FALSE(contains(r.value(), "alert(1)"));
}

TEST(MarkdownRendererTest, SanitizesJavascriptLinkFromMarkdown)
{
    auto r = render_markdown("[click](javascript:alert(1))\n");
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(contains(r.value(), "javascript:"));
}

TEST(MarkdownRendererTest, EmptyMarkdownYieldsEmptyOrTrivial)
{
    auto r = render_markdown("");
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(contains(r.value(), "<script"));
}

} // namespace
