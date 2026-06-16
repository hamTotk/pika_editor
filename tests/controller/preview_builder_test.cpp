// controller/preview_builder の検証（sprint5 must）。
// - CSP メタ（script-src https://app.pika のみ・base/form-action/frame-ancestors 'none'）の付与。
// - 差分の色非依存 +/- マーカ＋クラス（要件8.4。色だけに依存させない）。
// - プレビュー＋差分ON の grid（左プレビュー・右差分。design 6章）。
// - 差分原文の HTML エスケープ（ユーザー文書由来の XSS 経路を作らない）。
#include "controller/preview_builder.h"

#include "core/diff/diff_types.h"
#include "core/render/render_options.h"

#include <gtest/gtest.h>

namespace
{

using pika::controller::build_diff_body;
using pika::controller::build_diff_document;
using pika::controller::build_preview_diff_grid_document;
using pika::controller::build_preview_document;
using pika::controller::escape_html;
using pika::controller::PreviewDoc;
using pika::controller::PreviewKind;
using pika::core::diff::DiffLine;
using pika::core::diff::DiffResult;
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
    // base href は doc.pika 仮想ホスト（file:/// の抜け穴を作らない。design 6章）。
    EXPECT_TRUE(contains(html, "<base href=\"https://doc.pika/\">"));
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

} // namespace
