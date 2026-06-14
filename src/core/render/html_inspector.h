// core/render: HtmlInspector（JS 依存検知・外部リソース検知）。
// design.md 6章「HTMLプレビュー … JS検知（HtmlInspector）→通知バー＋既定のブラウザで開く」/
// 要件6.2・6.3。sprint4 must「<script>・Tailwind CDN を検知」「http(s) の外部参照を検知」。
//
// 検知のみ（除去はしない＝サニタイズと責務分離）。HTML プレビューは JS 無効で表示するが、
// JS 依存を検知したら通知バーで「既定のブラウザで開く」を促す。外部リソースを検知したら
// オプトイン導線を出す（要件2.4「文書を開いただけでは外部通信は一切起きない」）。
//
// 純ロジック（WebView2 非依存）。gtest で決定論検証する。
#pragma once

#include <string>
#include <string_view>

namespace pika::core::render
{

// 検知結果。フラグの組で UI 側の通知バー導線を出し分ける。
struct InspectionResult
{
    bool has_script = false;            // <script> タグを含む
    bool has_tailwind_cdn = false;      // Tailwind CDN 参照を含む
    bool has_external_resource = false; // http(s) の外部リソース参照を含む

    // JS 依存（script または Tailwind CDN）。通知バーで「既定のブラウザで開く」を促す。
    bool depends_on_js() const { return has_script || has_tailwind_cdn; }
};

// HTML を走査して JS 依存・外部リソース参照を検知する。
InspectionResult inspect_html(std::string_view html);

} // namespace pika::core::render
