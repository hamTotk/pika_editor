// core/render: ホワイトリスト方式 HTML サニタイザ。
// design.md 6章「文書由来HTMLはホワイトリスト方式サニタイズ（許可タグ・許可属性のみ。インライン
// SVG無害化・CSS url()/@import 遮断）」/ 要件6.2「<script>・イベント属性・javascript: URL・
// <iframe>/<object>/<embed>/<base> 等の除去」。
// sprint4 must: ホワイトリストサニタイズ・インラインSVG無害化・CSS遮断。
//
// 方針はホワイトリスト（許可したタグ・属性だけ通す）。未知タグは中身（テキスト）を残してタグ自体を
// 落とす（情報を黙って消さない）。危険タグ（script/iframe/object/embed/base/foreignObject 等）は
// 中身ごと落とす。属性は許可名のみ、かつ href/src は危険スキーム（javascript:/data: 等）なら除去。
// イベント属性（on*）は全除去。CSS（style 属性・<style> 内）は url()/@import を遮断する。
//
// CSP（csp_builder）と二重防御（design.md 6章 C6）。本サニタイザは CSP が無効化された環境でも
// 単体で XSS を防ぐ独立した壁である。純ロジック・gtest で決定論検証する。
#pragma once

#include <string>
#include <string_view>

namespace pika::core::render
{

// 文書由来 HTML（md4c 出力 ＋ Markdown 内 raw HTML）をサニタイズして安全な HTML を返す。
// 入力が壊れていても例外を投げず、可能な限り安全な出力に倒す（AI 出力＝非定型を主対象）。
std::string sanitize_html(std::string_view html);

} // namespace pika::core::render
