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

// 二重エスケープ回帰（F-004 Mermaid 図描画バグ）。md4c は ```mermaid フェンス内の `-->` を
// `--&gt;` に正しくエスケープして出力する。サニタイザはこの `&gt;` を `&amp;gt;` に再エスケープ
// してはならない（再エスケープすると preview-bootstrap.js が <code>.textContent から得るソースが
// `--&gt;` のまま mermaid へ渡り「Syntax error」になる）。
TEST(MarkdownRendererTest, MermaidFenceArrowNotDoubleEscaped)
{
    auto r = render_markdown("```mermaid\ngraph TD\nA[start] --> B{cond}\n```\n");
    ASSERT_TRUE(r.is_ok());
    const std::string& html = r.value();
    // mermaid コードブロックとして出る。
    EXPECT_TRUE(contains(html, "class=\"language-mermaid\""));
    // 矢印は 1 段エスケープの `--&gt;`（= textContent で `-->` に復号される）であること。
    EXPECT_TRUE(contains(html, "--&gt;"));
    // 二重エスケープ `&amp;gt;` が現れないこと（これが症状の根因）。
    EXPECT_FALSE(contains(html, "&amp;gt;"));
}

// インラインコード内の `<` `>` `&` は 1 段だけエスケープされること（二重エスケープ防止の一般化）。
TEST(MarkdownRendererTest, InlineCodeSpecialCharsNotDoubleEscaped)
{
    auto r = render_markdown("`a & b < c > d`\n");
    ASSERT_TRUE(r.is_ok());
    const std::string& html = r.value();
    EXPECT_TRUE(contains(html, "a &amp; b &lt; c &gt; d"));
    EXPECT_FALSE(contains(html, "&amp;amp;"));
    EXPECT_FALSE(contains(html, "&amp;lt;"));
    EXPECT_FALSE(contains(html, "&amp;gt;"));
}

// 本文中のアンパサンド（AT&T・裸の &）は 1 段だけ `&amp;` になり、二重化しないこと。
TEST(MarkdownRendererTest, AmpersandInTextEscapedOnce)
{
    auto r = render_markdown("AT&T and bare & here.\n");
    ASSERT_TRUE(r.is_ok());
    const std::string& html = r.value();
    EXPECT_TRUE(contains(html, "AT&amp;T"));
    EXPECT_FALSE(contains(html, "&amp;amp;"));
}

} // namespace
