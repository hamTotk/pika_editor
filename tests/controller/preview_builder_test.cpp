// controller/preview_builder の検証（sprint5 must）。
// - CSP メタ（script-src https://app.pika のみ・base/form-action/frame-ancestors 'none'）の付与。
// - 差分の色非依存 +/- マーカ＋クラス（要件8.4。色だけに依存させない）。
// - プレビュー＋差分ON の grid（左プレビュー・右差分。design 6章）。
// - 差分原文の HTML エスケープ（ユーザー文書由来の XSS 経路を作らない）。
#include "controller/preview_builder.h"

#include "core/diff/diff_types.h"
#include "core/render/markdown_renderer.h"
#include "core/render/preview_features.h"
#include "core/render/render_options.h"

#include <gtest/gtest.h>

namespace
{

using pika::controller::build_diff_body;
using pika::controller::build_diff_document;
using pika::controller::build_feature_scripts;
using pika::controller::build_preview_diff_grid_document;
using pika::controller::build_preview_document;
using pika::controller::classify_preview_kind;
using pika::controller::escape_html;
using pika::controller::is_diffable_type;
using pika::controller::PreviewDoc;
using pika::controller::PreviewKind;
using pika::core::diff::DiffLine;
using pika::core::diff::DiffResult;
using pika::core::diff::InlineSpan;
using pika::core::diff::LineOp;
using pika::core::render::RemoteResourcePolicy;

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// ---- HTML エスケープ ----

TEST(PreviewBuilderTest, EscapeHtmlEscapesMetacharacters)
{
    EXPECT_EQ(escape_html("<script>alert(\"x\")&</script>"),
              "&lt;script&gt;alert(&quot;x&quot;)&amp;&lt;/script&gt;");
}

TEST(PreviewBuilderTest, EscapePassesPlainTextUnchanged)
{
    EXPECT_EQ(escape_html("hello 日本語"), "hello 日本語");
}

// ---- CSP メタの付与（design 6章。既定 Blocked で外部 http(s) を含まない） ----

TEST(PreviewBuilderTest, PreviewDocumentHasCspMetaWithAppPikaScriptSrc)
{
    PreviewDoc doc;
    doc.kind = PreviewKind::Markdown;
    doc.body_html = "<h1>title</h1>";
    doc.remote_policy = RemoteResourcePolicy::Blocked;
    const std::string html = build_preview_document(doc);

    EXPECT_TRUE(contains(html, "Content-Security-Policy"));
    EXPECT_TRUE(contains(html, "script-src https://app.pika"));
    EXPECT_TRUE(contains(html, "base-uri 'none'"));
    EXPECT_TRUE(contains(html, "form-action 'none'"));
    EXPECT_TRUE(contains(html, "frame-ancestors 'none'"));
    // <base> 要素は出さない（CSP base-uri 'none' で無効化されるため。相対解決はページ URL
    // = https://doc.pika/ からのナビゲートが担う。base 方針を CSP に一本化。design 6章 C6）。
    EXPECT_FALSE(contains(html, "<base href"));
    // 同梱スタイルは app.pika 仮想ホストから配信する。
    EXPECT_TRUE(contains(html, "https://app.pika/preview.css"));
    // 既定（Blocked）では外部 http(s) を CSP に含めない。
    EXPECT_FALSE(contains(html, "img-src 'none' https:"));
    EXPECT_TRUE(contains(html, "<h1>title</h1>"));
}

TEST(PreviewBuilderTest, RemoteAllowedAddsExternalToCsp)
{
    PreviewDoc doc;
    doc.body_html = "<p>x</p>";
    doc.remote_policy = RemoteResourcePolicy::Allowed;
    const std::string html = build_preview_document(doc);
    // オプトイン時のみ img-src に外部スキームが乗る（script-src は不変＝同梱アセットのみ）。
    EXPECT_TRUE(contains(html, "https:"));
    EXPECT_TRUE(contains(html, "script-src https://app.pika"));
}

// ---- 差分の色非依存 +/- マーカ＋クラス ----

TEST(PreviewBuilderTest, DiffBodyEmitsColorIndependentMarkersAndClasses)
{
    DiffResult diff;
    diff.lines.push_back(DiffLine{LineOp::Context, "keep", {}, 1, 1});
    diff.lines.push_back(DiffLine{LineOp::Delete, "old", {}, 2, 0});
    diff.lines.push_back(DiffLine{LineOp::Add, "new", {}, 0, 2});

    const std::string body = build_diff_body(diff);
    // 行クラス（色非依存）。
    EXPECT_TRUE(contains(body, "diff-ctx"));
    EXPECT_TRUE(contains(body, "diff-del"));
    EXPECT_TRUE(contains(body, "diff-add"));
    // 行頭マーカ（記号でも判別＝要件8.4）。
    EXPECT_TRUE(contains(body, "diff-marker"));
    EXPECT_TRUE(contains(body, ">+<"));
    EXPECT_TRUE(contains(body, ">-<"));
}

TEST(PreviewBuilderTest, DiffBodyEscapesLineText)
{
    DiffResult diff;
    diff.lines.push_back(DiffLine{LineOp::Add, "<img onerror=x>", {}, 0, 1});
    const std::string body = build_diff_body(diff);
    // 原文 HTML はエスケープされ実 DOM にならない（XSS 経路を作らない）。
    EXPECT_TRUE(contains(body, "&lt;img onerror=x&gt;"));
    EXPECT_FALSE(contains(body, "<img onerror=x>"));
}

TEST(PreviewBuilderTest, TruncatedDiffShowsMinimalInfoOnly)
{
    DiffResult diff;
    diff.truncated = true;
    const std::string body = build_diff_body(diff);
    EXPECT_TRUE(contains(body, "diff-truncated"));
    // 大きすぎる差分は中身を出さない（diff-line を生成しない）。
    EXPECT_FALSE(contains(body, "diff-line"));
}

// ---- 差分文書（ソース＋差分ON / 分割＋差分ON の差分面） ----

TEST(PreviewBuilderTest, DiffDocumentIsFullHtmlWithCsp)
{
    DiffResult diff;
    diff.lines.push_back(DiffLine{LineOp::Add, "a", {}, 0, 1});
    const std::string html = build_diff_document(diff, RemoteResourcePolicy::Blocked);
    EXPECT_TRUE(contains(html, "<!DOCTYPE html>"));
    EXPECT_TRUE(contains(html, "Content-Security-Policy"));
    EXPECT_TRUE(contains(html, "diff-view"));
    EXPECT_TRUE(contains(html, "diff-add"));
}

// ---- プレビュー＋差分ON の grid（左プレビュー・右差分） ----

TEST(PreviewBuilderTest, PreviewDiffGridHasBothPanes)
{
    PreviewDoc doc;
    doc.body_html = "<h2>doc</h2>";
    DiffResult diff;
    diff.lines.push_back(DiffLine{LineOp::Delete, "gone", {}, 1, 0});

    const std::string html = build_preview_diff_grid_document(doc, diff);
    EXPECT_TRUE(contains(html, "preview-diff-grid"));
    EXPECT_TRUE(contains(html, "grid-left"));
    EXPECT_TRUE(contains(html, "grid-right"));
    // 左にプレビュー本文、右に差分。
    EXPECT_TRUE(contains(html, "<h2>doc</h2>"));
    EXPECT_TRUE(contains(html, "diff-del"));
    // 1 枚の完全HTML文書（共有 WebView2 へそのままナビゲート）。
    EXPECT_TRUE(contains(html, "<!DOCTYPE html>"));
    EXPECT_TRUE(contains(html, "Content-Security-Policy"));
}

// 同一入力で同一出力（純粋）。
TEST(PreviewBuilderTest, BuildIsDeterministic)
{
    PreviewDoc doc;
    doc.body_html = "<p>same</p>";
    EXPECT_EQ(build_preview_document(doc), build_preview_document(doc));
}

// ---- プレビュー種別の分類（JS 有効/無効の出し分け根拠。design 6章 C5） ----

TEST(PreviewBuilderTest, ClassifyPreviewKindFromExtension)
{
    // .md/.markdown は Markdown（JS 有効）。
    EXPECT_EQ(classify_preview_kind("readme.md"), PreviewKind::Markdown);
    EXPECT_EQ(classify_preview_kind("notes.MARKDOWN"), PreviewKind::Markdown);
    // .html/.htm は HTML（JS 無効＝IsScriptEnabled=false 相当。ユーザー文書由来 JS を実行しない）。
    EXPECT_EQ(classify_preview_kind("page.html"), PreviewKind::Html);
    EXPECT_EQ(classify_preview_kind("page.HTM"), PreviewKind::Html);
    // フルパス（'/' '\\' 区切り）でも末尾名の拡張子で判定する。
    EXPECT_EQ(classify_preview_kind("C:\\docs\\index.html"), PreviewKind::Html);
    EXPECT_EQ(classify_preview_kind("a/b/c.md"), PreviewKind::Markdown);
    // 分類不能（拡張子なし・未知）は Markdown 相当（pika 生成の信頼済み HTML）として扱う。
    EXPECT_EQ(classify_preview_kind("LICENSE"), PreviewKind::Markdown);
}

TEST(PreviewBuilderTest, IsDiffableTypeAcceptsMdHtmlSvg)
{
    EXPECT_TRUE(is_diffable_type("a.md"));
    EXPECT_TRUE(is_diffable_type("a.markdown"));
    EXPECT_TRUE(is_diffable_type("a.html"));
    EXPECT_TRUE(is_diffable_type("a.htm"));
    EXPECT_TRUE(is_diffable_type("a.svg"));
    // 画像・バイナリ・未知は差分対象でない（NotDiffableType の入力になる）。
    EXPECT_FALSE(is_diffable_type("a.png"));
    EXPECT_FALSE(is_diffable_type("a.bin"));
    EXPECT_FALSE(is_diffable_type("noext"));
}

// ---- 行内強調（DiffLine::spans）の HTML 出力（色＋記号に加える行内チャンネル。要件8.4） ----

TEST(PreviewBuilderTest, DiffBodyWrapsInlineSpans)
{
    DiffResult diff;
    // "hello world" のうち [6,11) = "world" を変更語として強調する。
    DiffLine line{LineOp::Add, "hello world", {}, 0, 1};
    line.spans.push_back(InlineSpan{6, 11});
    diff.lines.push_back(line);

    const std::string body = build_diff_body(diff);
    // 行内強調は <span class="diff-word"> で囲み、変更語だけをラップする。
    EXPECT_TRUE(contains(body, "<span class=\"diff-word\">world</span>"));
    // 強調外の部分はそのまま出る。
    EXPECT_TRUE(contains(body, "hello "));
}

TEST(PreviewBuilderTest, DiffBodyInlineSpanEscapesContent)
{
    DiffResult diff;
    // 強調区間の中身もエスケープする（XSS 経路を作らない）。"<b>" を強調。
    DiffLine line{LineOp::Add, "a<b>c", {}, 0, 1};
    line.spans.push_back(InlineSpan{1, 4}); // "<b>"
    diff.lines.push_back(line);

    const std::string body = build_diff_body(diff);
    EXPECT_TRUE(contains(body, "<span class=\"diff-word\">&lt;b&gt;</span>"));
    EXPECT_FALSE(contains(body, "<b>c"));
}

TEST(PreviewBuilderTest, DiffBodyIgnoresOutOfRangeSpans)
{
    DiffResult diff;
    // 範囲外/逆転 span は無視し、行が消えたりクラッシュしたりしない（堅牢化）。
    DiffLine line{LineOp::Add, "abc", {}, 0, 1};
    line.spans.push_back(InlineSpan{5, 9}); // 範囲外
    line.spans.push_back(InlineSpan{2, 1}); // 逆転
    diff.lines.push_back(line);

    const std::string body = build_diff_body(diff);
    EXPECT_TRUE(contains(body, "abc"));
    EXPECT_FALSE(contains(body, "diff-word"));
}

// ---- F-004 同梱スクリプト/CSS の条件付き注入（該当記法がある時だけ＝未使用時コストゼロ） ---- //

using pika::core::render::PreviewFeatures;

TEST(PreviewBuilderTest, NoFeaturesInjectsNoVendorScriptsOrLinks)
{
    PreviewDoc doc;
    doc.body_html = "<p>plain</p>";
    // features は既定（全 false）。
    const std::string html = build_preview_document(doc);
    // vendor スクリプト・ブートストラップ・vendor CSS を一切出さない（素のプレビューと同コスト）。
    EXPECT_FALSE(contains(html, "vendor/mermaid.min.js"));
    EXPECT_FALSE(contains(html, "vendor/katex.min.js"));
    EXPECT_FALSE(contains(html, "vendor/highlight.min.js"));
    EXPECT_FALSE(contains(html, "vendor/katex.min.css"));
    EXPECT_FALSE(contains(html, "vendor/hljs-github"));
    EXPECT_FALSE(contains(html, "preview-bootstrap.js"));
    // preview.css は常に出る（基本スタイル）。
    EXPECT_TRUE(contains(html, "https://app.pika/preview.css"));
}

TEST(PreviewBuilderTest, MermaidFeatureInjectsMermaidScriptOnly)
{
    PreviewDoc doc;
    doc.body_html = "<pre><code class=\"language-mermaid\">graph TD; A--&gt;B;</code></pre>";
    doc.features.mermaid = true;
    const std::string html = build_preview_document(doc);
    EXPECT_TRUE(contains(html, "https://app.pika/vendor/mermaid.min.js"));
    EXPECT_TRUE(contains(html, "https://app.pika/preview-bootstrap.js"));
    // 数式・コードは未検出なのでそれらの vendor は出さない。
    EXPECT_FALSE(contains(html, "vendor/katex.min.js"));
    EXPECT_FALSE(contains(html, "vendor/highlight.min.js"));
}

TEST(PreviewBuilderTest, MathFeatureInjectsKatexScriptAndCss)
{
    PreviewDoc doc;
    doc.body_html = "<p>$x$</p>";
    doc.features.math = true;
    const std::string html = build_preview_document(doc);
    EXPECT_TRUE(contains(html, "https://app.pika/vendor/katex.min.js"));
    EXPECT_TRUE(contains(html, "https://app.pika/vendor/katex-auto-render.min.js"));
    EXPECT_TRUE(contains(html, "https://app.pika/vendor/katex.min.css")); // <head> に CSS も出る。
    EXPECT_TRUE(contains(html, "https://app.pika/preview-bootstrap.js"));
    EXPECT_FALSE(contains(html, "vendor/mermaid.min.js"));
}

TEST(PreviewBuilderTest, CodeFeatureInjectsHighlightScriptAndThemeCss)
{
    PreviewDoc doc;
    doc.body_html = "<pre><code class=\"language-cpp\">int x;</code></pre>";
    doc.features.code = true;
    const std::string html = build_preview_document(doc);
    EXPECT_TRUE(contains(html, "https://app.pika/vendor/highlight.min.js"));
    EXPECT_TRUE(contains(html, "https://app.pika/vendor/hljs-github.min.css"));
    // ダークテーマは media クエリ付きで上書き（テーマ追従）。
    EXPECT_TRUE(contains(html, "prefers-color-scheme: dark"));
    EXPECT_TRUE(contains(html, "https://app.pika/vendor/hljs-github-dark.min.css"));
    EXPECT_TRUE(contains(html, "https://app.pika/preview-bootstrap.js"));
}

TEST(PreviewBuilderTest, AllFeaturesInjectAllVendorAssets)
{
    PreviewDoc doc;
    doc.body_html = "<p>x</p>";
    doc.features.mermaid = true;
    doc.features.math = true;
    doc.features.code = true;
    const std::string html = build_preview_document(doc);
    EXPECT_TRUE(contains(html, "vendor/mermaid.min.js"));
    EXPECT_TRUE(contains(html, "vendor/katex.min.js"));
    EXPECT_TRUE(contains(html, "vendor/highlight.min.js"));
    EXPECT_TRUE(contains(html, "vendor/katex.min.css"));
    EXPECT_TRUE(contains(html, "vendor/hljs-github.min.css"));
    EXPECT_TRUE(contains(html, "preview-bootstrap.js"));
}

TEST(PreviewBuilderTest, FeatureScriptsAreAllAppPikaHosted)
{
    // 注入する <script src> はすべて app.pika 仮想ホスト（CSP script-src https://app.pika
    // に合致）。
    PreviewFeatures feat;
    feat.mermaid = true;
    feat.math = true;
    feat.code = true;
    const std::string scripts = build_feature_scripts(feat);
    // 各 <script src= の直後は必ず https://app.pika/ で始まる（外部 CDN 等を出さない＝CSP
    // に通す）。
    std::size_t pos = 0;
    int count = 0;
    const std::string needle = "<script src=\"";
    while ((pos = scripts.find(needle, pos)) != std::string::npos)
    {
        const std::size_t v = pos + needle.size();
        EXPECT_EQ(scripts.compare(v, std::char_traits<char>::length("https://app.pika/"),
                                  "https://app.pika/"),
                  0);
        ++count;
        pos = v;
    }
    EXPECT_GT(count, 0);
}

TEST(PreviewBuilderTest, EmptyFeatureScriptsForNoFeatures)
{
    EXPECT_EQ(build_feature_scripts(PreviewFeatures{}), std::string());
}

TEST(PreviewBuilderTest, GridDocumentInjectsFeatureScriptsForPreviewSide)
{
    // プレビュー＋差分ON（grid）でも左プレビュー側に同梱スクリプトを注入する（design 6章）。
    PreviewDoc doc;
    doc.body_html = "<p>$x$</p>";
    doc.features.math = true;
    DiffResult diff;
    diff.lines.push_back(DiffLine{LineOp::Add, "a", {}, 0, 1});
    const std::string html = build_preview_diff_grid_document(doc, diff);
    EXPECT_TRUE(contains(html, "vendor/katex.min.js"));
    EXPECT_TRUE(contains(html, "preview-bootstrap.js"));
}

// ---- サニタイズとの相互作用：ユーザー文書由来 script は除去・pika 注入の vendor は保持 ---- //

TEST(PreviewBuilderTest, UserScriptRemovedButInjectedVendorScriptsKept)
{
    using pika::core::render::detect_preview_features;
    using pika::core::render::render_markdown;
    // ユーザー Markdown 内に raw <script> と言語付きコードブロックを含める。
    const std::string md = "<script>alert('xss')</script>\n\n```js\nlet x = 1;\n```\n";
    const auto rendered = render_markdown(md);
    ASSERT_TRUE(rendered.is_ok());

    PreviewDoc doc;
    doc.body_html = rendered.value();           // サニタイズ済み本文（render_markdown が通す）。
    doc.features = detect_preview_features(md); // code=true を検出して highlight.js を注入。

    const std::string html = build_preview_document(doc);
    // ユーザー文書由来の <script>alert は本文から除去されている（サニタイズ済み）。
    EXPECT_FALSE(contains(html, "alert('xss')"));
    EXPECT_FALSE(contains(html, "<script>alert"));
    // pika 注入の信頼済み vendor スクリプトは残る（app.pika 配信）。
    EXPECT_TRUE(contains(html, "https://app.pika/vendor/highlight.min.js"));
    EXPECT_TRUE(contains(html, "https://app.pika/preview-bootstrap.js"));
}

} // namespace
