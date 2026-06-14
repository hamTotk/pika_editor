// core/render: プレビュー描画の共有オプションとガード閾値。
// design.md 6章「WebView2の使い方（安全方針の実装）」/ 要件2.2「レンダリング暴走ガード」・
// 6章「プレビュー」。sprint4 はこの設定群を入力に、UI/WebView2 非依存で
// サニタイズ・検知・CSP 組み立て・ガード判定を決定論実装する。
//
// 閾値はすべて設定で変更可能（要件2.2「上限はすべて設定で緩和できる」）。既定値は要件の数値。
#pragma once

#include <cstddef>
#include <cstdint>

namespace pika::core::render
{

// リモートリソース取得の許可状態（要件2.4・6.2「既定オフ（オプトイン）」）。
// 既定はオフ。CspBuilder はオフのとき外部 http(s) を CSP に含めない。
enum class RemoteResourcePolicy
{
    Blocked = 0, // 既定。外部 http(s) を遮断する
    Allowed, // ユーザーがオプトインした間のみ。img-src/font-src/style-src に http: https: を追加
};

// レンダリング暴走ガードの閾値（要件2.2。既定値は要件の数値そのまま）。
// AI 出力＝非定型を主対象とするため既定オン。超過入力はレンダリングせず外部アプリへ誘導する。
struct RenderGuardLimits
{
    // 画像の総ピクセル数上限（既定6000万px）。
    std::uint64_t max_image_pixels = 60'000'000ull;
    // SVG の展開ピクセル数相当の上限（既定8000万px）。
    std::uint64_t max_svg_pixels = 80'000'000ull;
    // SVG の要素数上限（既定5万要素）。
    std::size_t max_svg_elements = 50'000u;
    // HTML の要素数上限（暴走防止。閾値超過で開始前に不可判定する）。
    std::size_t max_html_elements = 500'000u;
    // HTML のネスト深さ上限（深い入れ子による描画爆発の防止）。
    std::size_t max_html_nest_depth = 512u;
};

} // namespace pika::core::render
