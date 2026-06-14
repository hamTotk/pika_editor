// core/render: CSP（Content-Security-Policy）テンプレートの組み立て。
// design.md 6章「CSPテンプレート」/ 要件2.4・6.2「リモートリソース既定オフ（オプトイン）」。
// sprint4 must「CSP テンプレートが script-src を https://app.pika のみに限定し、
// リモート許可オフ時に外部 http(s) を含まない文字列を生成する」。
//
// 一元定義（Markdownプレビュー／差分／SVG／HTMLプレビュー共通）。リモート許可オン時のみ
// img-src/font-src/style-src に http: https: を追加する。script-src は常に https://app.pika のみ
// （ユーザー文書由来 JS を実行しない境界。同梱の信頼済み JS だけが app.pika から動く）。
//
// 純ロジック（WebView2 非依存）。gtest で生成文字列を決定論検証する。
#pragma once

#include "core/render/render_options.h"

#include <string>

namespace pika::core::render
{

// CSP ヘッダ値（meta http-equiv または応答ヘッダに載せる 1 行）を組み立てる。
// policy=Blocked（既定）では外部 http(s) を一切含めない。policy=Allowed のときだけ
// img-src/font-src/style-src に http: https: を足す（script-src は不変＝同梱アセットのみ）。
std::string build_csp(RemoteResourcePolicy policy);

} // namespace pika::core::render
