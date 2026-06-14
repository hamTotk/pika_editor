// core/diff: 行内の単語/文字単位ハイライト計算。
// design.md 8章「行内強調：変更行ペアに対して (1) 空白区切りトークンでLCS →
// (2) トークン境界が取れない場合（日本語等、1トークンが行の大半を占める場合）は文字単位LCSへ
// フォールバック」。要件8.2。sprint6 must「空白区切りトークンの LCS で変更語を特定し、
// トークン境界が取れない日本語等では文字単位 LCS にフォールバックする」。
//
// 純ロジック（UI 非依存）。gtest で日本語フォールバック・空白区切りを決定論検証する。
#pragma once

#include "core/diff/diff_types.h"

#include <string_view>
#include <vector>

namespace pika::core::diff
{

// 削除行（old）と追加行（new）のペアに対し、各行の変更範囲をバイトオフセット区間で返す。
// out_old には old 行で削除された語/文字の区間、out_new には new 行で追加された区間が入る。
//
// 戦略（design.md 8章）:
//   1. 空白区切りトークンの LCS を取り、共通でないトークンを変更範囲とする。
//   2. トークン境界が取れない（= 行が単一トークンに偏る。日本語等で空白が無い）場合は、
//      UTF-8 コードポイント単位の LCS にフォールバックする。
// 返す区間は UTF-8 のコードポイント境界を跨がない（文字を割らない）。
void compute_inline_spans(std::string_view old_line, std::string_view new_line,
                          std::vector<InlineSpan>& out_old, std::vector<InlineSpan>& out_new);

} // namespace pika::core::diff
