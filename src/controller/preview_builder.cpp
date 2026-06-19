#include "controller/preview_builder.h"

#include "core/render/csp_builder.h"

#include <algorithm>
#include <cctype>

namespace pika::controller
{

namespace
{

// 名前/パスから小文字化した拡張子（先頭ドット除く）を取り出す。区切りは '/' と '\\' 両対応。
std::string lower_ext(std::string_view name_or_path)
{
    std::size_t slash = name_or_path.find_last_of("/\\");
    std::string_view name =
        (slash == std::string_view::npos) ? name_or_path : name_or_path.substr(slash + 1);
    std::size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size())
    {
        return {};
    }
    std::string ext(name.substr(dot + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// 行頭マーカ文字（色非依存の判別。DiffLine::marker と同義だが HTML 側で確実に出す）。
char line_marker(core::diff::LineOp op)
{
    switch (op)
    {
    case core::diff::LineOp::Add:
        return '+';
    case core::diff::LineOp::Delete:
        return '-';
    case core::diff::LineOp::Context:
    default:
        return ' ';
    }
}

// 行種別の色非依存クラス名（CSS が色＋背景を付ける。記号と二重化＝要件8.4）。
std::string_view line_class(core::diff::LineOp op)
{
    switch (op)
    {
    case core::diff::LineOp::Add:
        return "diff-add";
    case core::diff::LineOp::Delete:
        return "diff-del";
    case core::diff::LineOp::Context:
    default:
        return "diff-ctx";
    }
}

} // namespace

PreviewKind classify_preview_kind(std::string_view name_or_path)
{
    const std::string ext = lower_ext(name_or_path);
    if (ext == "html" || ext == "htm")
    {
        // HTML プレビューは JS 無効（ユーザー文書由来の JS を実行しない。design 6章 C5）。
        return PreviewKind::Html;
    }
    // .md/.markdown ほか pika 生成の信頼済み HTML は JS 有効（同梱アセットのみ）。
    return PreviewKind::Markdown;
}

bool is_diffable_type(std::string_view name_or_path)
{
    const std::string ext = lower_ext(name_or_path);
    return ext == "md" || ext == "markdown" || ext == "html" || ext == "htm" || ext == "svg";
}

std::string escape_html(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

namespace
{

// 行内強調区間（DiffLine::spans）を <span class="diff-word"> でラップして HTML 化する。
// spans はバイトオフセット（UTF-8 境界。core/diff が保証）。色＋記号に加え行内チャンネルを出す
// （色非依存の多重符号化。要件8.4 / eval medium「行内下線」）。範囲外/逆転 span は無視する。
std::string build_inline_text(std::string_view text,
                              const std::vector<core::diff::InlineSpan>& spans)
{
    if (spans.empty())
    {
        return escape_html(text);
    }
    // begin 昇順・非重複を前提に、区間内/外を切り替えながらエスケープして連結する。
    std::vector<core::diff::InlineSpan> sorted(spans.begin(), spans.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const core::diff::InlineSpan& a, const core::diff::InlineSpan& b) {
                  return a.begin < b.begin;
              });

    std::string out;
    std::size_t pos = 0;
    for (const auto& s : sorted)
    {
        if (s.begin >= s.end || s.begin >= text.size())
        {
            continue; // 空/逆転/範囲外は捨てる（堅牢化）。
        }
        const std::size_t b = std::max(s.begin, pos);
        const std::size_t e = std::min(s.end, text.size());
        if (b >= e)
        {
            continue; // 直前 span と重なり切った。
        }
        if (b > pos)
        {
            out += escape_html(text.substr(pos, b - pos));
        }
        out += "<span class=\"diff-word\">";
        out += escape_html(text.substr(b, e - b));
        out += "</span>";
        pos = e;
    }
    if (pos < text.size())
    {
        out += escape_html(text.substr(pos));
    }
    return out;
}

} // namespace

std::string build_feature_head_links(const core::render::PreviewFeatures& features)
{
    // 該当機能の CSS だけを <head> に足す（未使用時は何も出さない＝コストゼロ。design
    // 6章・原則③）。 すべて app.pika 仮想ホストから配信する（CSP style-src
    // https://app.pika。ユーザー文書外）。
    std::string out;
    if (features.math)
    {
        // KaTeX のスタイル。katex.min.css は fonts/*.woff2 を相対参照するため、CSS 配信元
        // （app.pika/vendor/）基準で app.pika/vendor/fonts/*.woff2 に解決される（font-src
        // app.pika）。
        out += "<link rel=\"stylesheet\" href=\"https://app.pika/vendor/katex.min.css\">";
    }
    if (features.code)
    {
        // highlight.js テーマ。ライト既定＋ダークは media クエリで上書き（preview.css
        // と同じテーマ追従）。
        out += "<link rel=\"stylesheet\" href=\"https://app.pika/vendor/hljs-github.min.css\">";
        out += "<link rel=\"stylesheet\" media=\"(prefers-color-scheme: dark)\" "
               "href=\"https://app.pika/vendor/hljs-github-dark.min.css\">";
    }
    return out;
}

std::string build_head(core::render::RemoteResourcePolicy policy,
                       const core::render::PreviewFeatures& features)
{
    std::string head;
    head += "<head>";
    head += "<meta charset=\"utf-8\">";
    head += "<meta http-equiv=\"Content-Security-Policy\" content=\"";
    head += core::render::build_csp(policy);
    head += "\">";
    // <base> 要素は出さない（CSP base-uri 'none' で無効化される＝二重防御。design 6章 C6）。
    // 相対画像/リンクの解決は「ページ URL = https://doc.pika/ からのナビゲート」が担う
    // （SetPage の baseUrl を preview_view が指定）。base 方針を CSP に一本化する。
    head += "<link rel=\"stylesheet\" href=\"https://app.pika/preview.css\">";
    // 該当記法があるときだけ vendor CSS を足す（未使用時コストゼロ。design 6章・原則③）。
    head += build_feature_head_links(features);
    head += "</head>";
    return head;
}

std::string build_feature_scripts(const core::render::PreviewFeatures& features)
{
    if (!features.any())
    {
        return {}; // 一切注入しない（素の Markdown プレビューと同コスト。原則③）。
    }
    // 読み込み順を担保する：vendor ライブラリ（同期 <script src>＝記述順に実行）→
    // ブートストラップ。 すべて app.pika から配信（CSP script-src
    // https://app.pika。ユーザー文書由来 JS は実行しない）。
    std::string out;
    if (features.math)
    {
        out += "<script src=\"https://app.pika/vendor/katex.min.js\"></script>";
        out += "<script src=\"https://app.pika/vendor/katex-auto-render.min.js\"></script>";
    }
    if (features.code)
    {
        out += "<script src=\"https://app.pika/vendor/highlight.min.js\"></script>";
    }
    if (features.mermaid)
    {
        out += "<script src=\"https://app.pika/vendor/mermaid.min.js\"></script>";
    }
    // 最後に pika 自作のブートストラップ（per-block
    // try/catch＋1秒タイムアウト＋失敗件数通知。I1）。
    out += "<script src=\"https://app.pika/preview-bootstrap.js\"></script>";
    return out;
}

std::string build_diff_body(const core::diff::DiffResult& diff)
{
    std::string body;
    body += "<div class=\"diff\">";
    if (diff.truncated)
    {
        // 大規模入力は差分を計算しない（要件8章）。最小情報のみ（内容は出さない）。
        body += "<div class=\"diff-truncated\">差分が大きすぎるため表示を省略しました</div>";
        body += "</div>";
        return body;
    }
    for (const auto& line : diff.lines)
    {
        body += "<div class=\"diff-line ";
        body += line_class(line.op);
        body += "\">";
        // 行頭マーカ（色に依存しない判別記号）を必ず出す。
        body += "<span class=\"diff-marker\">";
        body += line_marker(line.op);
        body += "</span>";
        body += "<span class=\"diff-text\">";
        body += build_inline_text(line.text, line.spans);
        body += "</span>";
        body += "</div>";
    }
    body += "</div>";
    return body;
}

std::string build_preview_document(const PreviewDoc& doc)
{
    std::string html;
    html += "<!DOCTYPE html><html lang=\"ja\">";
    html += build_head(doc.remote_policy, doc.features);
    html += "<body class=\"preview\">";
    html += doc.body_html;
    // 該当記法があるときだけ同梱スクリプトを </body> 直前に注入する（design 6章・原則③）。
    html += build_feature_scripts(doc.features);
    html += "</body></html>";
    return html;
}

std::string build_diff_document(const core::diff::DiffResult& diff,
                                core::render::RemoteResourcePolicy policy)
{
    std::string html;
    html += "<!DOCTYPE html><html lang=\"ja\">";
    html += build_head(policy);
    html += "<body class=\"diff-view\">";
    html += build_diff_body(diff);
    html += "</body></html>";
    return html;
}

std::string build_preview_diff_grid_document(const PreviewDoc& doc,
                                             const core::diff::DiffResult& diff)
{
    std::string html;
    html += "<!DOCTYPE html><html lang=\"ja\">";
    html += build_head(doc.remote_policy, doc.features);
    // 左プレビュー・右差分を grid で横並び（1枚WebView2内。design 6章）。独立スクロールは CSS。
    html += "<body class=\"preview-diff-grid\">";
    html += "<div class=\"grid-left preview\">";
    html += doc.body_html;
    html += "</div>";
    html += "<div class=\"grid-right diff-view\">";
    html += build_diff_body(diff);
    html += "</div>";
    // 左プレビューに対して同梱スクリプトを適用する（差分面は素のテキスト。該当時のみ。design
    // 6章）。
    html += build_feature_scripts(doc.features);
    html += "</body></html>";
    return html;
}

} // namespace pika::controller
