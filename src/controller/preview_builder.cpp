#include "controller/preview_builder.h"

#include "core/render/csp_builder.h"

namespace pika::controller
{

namespace
{

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

std::string build_head(core::render::RemoteResourcePolicy policy)
{
    std::string head;
    head += "<head>";
    head += "<meta charset=\"utf-8\">";
    head += "<meta http-equiv=\"Content-Security-Policy\" content=\"";
    head += core::render::build_csp(policy);
    head += "\">";
    // 相対画像/リンクは doc.pika 仮想ホスト経由で解決する（design 6章。file:/// を作らない）。
    head += "<base href=\"https://doc.pika/\">";
    head += "<link rel=\"stylesheet\" href=\"https://app.pika/preview.css\">";
    head += "</head>";
    return head;
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
        body += escape_html(line.text);
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
    html += build_head(doc.remote_policy);
    html += "<body class=\"preview\">";
    html += doc.body_html;
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
    html += build_head(doc.remote_policy);
    // 左プレビュー・右差分を grid で横並び（1枚WebView2内。design 6章）。独立スクロールは CSS。
    html += "<body class=\"preview-diff-grid\">";
    html += "<div class=\"grid-left preview\">";
    html += doc.body_html;
    html += "</div>";
    html += "<div class=\"grid-right diff-view\">";
    html += build_diff_body(diff);
    html += "</div>";
    html += "</body></html>";
    return html;
}

} // namespace pika::controller
