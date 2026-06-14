// core/render: レンダリング暴走ガード判定（要件2.2）。
// design.md 1章「固まらない」/ 6章「要素数・展開ピクセル数がガード上限を超える入力は
// レンダリングせず外部アプリへ誘導」。sprint4 must「画像6000万px・SVG展開8000万px相当/
// 要素5万・HTML要素数/ネスト深さの閾値超過を入力サイズから開始前に判定し、超過ならレンダリング
// 不可フラグを返す」。
//
// 判定は「描画を開始する前」に入力サイズ（ピクセル数・要素数・ネスト深さ）から行い、超過なら
// レンダリングしない（中途で止めるのではなく、開始前に断る＝固まらない）。タイムアウト（10秒）は
// WebView 内 JS 側の責務でここでは扱わない（本モジュールは開始前の静的判定に専念）。
//
// 純ロジック（WebView2 非依存）。gtest で閾値前後の挙動を決定論検証する。
#pragma once

#include "core/render/render_options.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace pika::core::render
{

// ガード判定の結果。allowed=false のとき UI は通知バーで「既定のブラウザ/アプリで開く」へ誘導する。
struct RenderGuardVerdict
{
    bool allowed = true;
    // 不可理由（診断・通知文言の出し分け用。内容は載せない＝診断ログ方針と整合）。
    std::string reason;
};

// 画像の総ピクセル数（width*height）がガード上限を超えていないか開始前に判定する。
// width または height が 0、もしくは width*height がオーバーフローする巨大値は不可（安全側）。
RenderGuardVerdict guard_image(std::uint64_t width, std::uint64_t height,
                               const RenderGuardLimits& limits);

// SVG の展開ピクセル数相当（viewport の width*height）と要素数を判定する。
// いずれか一方でも上限超過なら不可。
RenderGuardVerdict guard_svg(std::uint64_t width, std::uint64_t height, std::size_t element_count,
                             const RenderGuardLimits& limits);

// HTML の要素数とネスト深さがガード上限を超えていないか判定する。
// いずれか一方でも超過すれば不可。要素数・深さは呼び出し側（軽量スキャン）が数える。
RenderGuardVerdict guard_html(std::size_t element_count, std::size_t nest_depth,
                              const RenderGuardLimits& limits);

} // namespace pika::core::render
