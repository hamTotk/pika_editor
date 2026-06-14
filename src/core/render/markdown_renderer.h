// core/render: Markdown→HTML 変換（md4c GFM）＋ホワイトリストサニタイズ。
// design.md 5.5「md4cでHTML化 → ホワイトリスト方式サニタイズ」/ 要件6.2「md4cによるGFM準拠
// （テーブル・タスクリスト・打消し線・自動リンク）」。
// sprint4 should「md4c による Markdown→HTML 変換（GFM）が呼び出せる」。
//
// md4c は <body> 内容のみを生成する。本ラッパは GFM ダイアレクトで変換し、結果を必ず
// sanitize_html に通してから返す（生 md4c 出力＝Markdown 内 raw HTML を含みうるため、
// サニタイズ前の HTML を外へ出さない）。テンプレート合成（CSP・base・スクリプト挿入）は
// UI 側の責務で、本モジュールは本文 HTML（サニタイズ済み）までを返す。
//
// 純ロジック（WebView2 非依存）。gtest で GFM 要素の生成とサニタイズ適用を検証する。
#pragma once

#include "util/result.h"

#include <string>
#include <string_view>

namespace pika::core::render
{

// Markdown を GFM で HTML 化し、サニタイズ済み本文 HTML を返す。
// md4c の変換失敗時は Result のエラー（例外は投げない＝コア公開 API は Result 方式）。
pika::util::Result<std::string> render_markdown(std::string_view markdown);

} // namespace pika::core::render
