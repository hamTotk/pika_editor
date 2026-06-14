// core/diff: 行差分（dtl/Myers）。
// design.md 8章「行差分：dtl（Myers）。入力はベースライン内容と現在内容の行分割（比較・ハッシュ
// とも LF 正規化後。表示は原文準拠）」。要件8章。sprint6 must「ベースライン内容 vs 現在内容を
// LF正規化後に行分割して Myers 差分を計算し、追加/削除/変更行を返す」。
//
// 純ロジック（UI 非依存）。gtest で改行のみ差・全行追加/削除・色非依存記号を決定論検証する。
#pragma once

#include "core/diff/diff_types.h"

#include <string>
#include <string_view>
#include <vector>

namespace pika::core::diff
{

// 内容を行に分割する（LF 正規化後）。CRLF→LF に畳んでから '\n' で区切る。
// 末尾改行直後の空行は行として数えない（"a\n" は 1 行 "a"）。空文字列は 0 行。
// 返す各行は改行を含まない原文の行内容。
std::vector<std::string> split_lines_lf(std::string_view content);

// ベースライン（old）と現在（new）の行差分を Myers（dtl）で計算する。
// LF 正規化後に行分割して照合するため、CRLF/LF のみ異なる同一内容は差分が空になる
// （要件8章 / design.md 8章「改行のみの差は出さない」）。
// 返す DiffLine 列は unified 順（Delete を先、Add を後に並べる）で、各行に色非依存の op を持つ。
// 行内強調（spans）はここでは付けない（変更行ペアの対応付けと併せて DiffEngine が付与する）。
std::vector<DiffLine> diff_lines(const std::vector<std::string>& old_lines,
                                 const std::vector<std::string>& new_lines);

} // namespace pika::core::diff
