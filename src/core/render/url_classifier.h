// core/render: URL 属性値の正規化と危険スキーム/外部スキーム判定。
// design.md 6章「javascript: URL の除去」「外部 http(s) の遮断」。HtmlSanitizer と HtmlInspector が
// 共通で使う。AI 出力の難読化（HTML エンティティ・前後空白・制御文字・大文字小文字）に頑健に倒す。
//
// 純ロジック。gtest で難読化パターンを決定論検証する。
#pragma once

#include <string>
#include <string_view>

namespace pika::core::render
{

// URL 値からスキーム接頭辞を抽出するための正規化を行う。
// - HTML エンティティ（&#106; / &#x6a; / &colon; など）を該当文字へ展開する（スキーム偽装対策）
// - 先頭の空白・タブ・改行・NUL 等の制御文字を除去する
// - 小文字化する
// 値全体ではなく「スキーム判定に十分な先頭部分」を返す（性能と安全性の両立）。
std::string normalize_url_scheme_probe(std::string_view raw);

// javascript: / vbscript: / data: 中の実行可能 MIME など、スクリプト実行に至るスキームか。
// 正規化後に判定する。href/src 等のナビゲーション系属性に適用する。
bool is_dangerous_url(std::string_view raw);

// http: または https:（外部参照）で始まるか。プロトコル相対 //host も外部とみなす。
bool is_external_url(std::string_view raw);

} // namespace pika::core::render
