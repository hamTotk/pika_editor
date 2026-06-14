#include "core/diff/diff_engine.h"

#include "core/diff/inline_diff.h"
#include "core/diff/line_diff.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pika::core::diff
{

namespace
{

// 入力サイズがガード上限を超えていないか開始前に判定する（design.md 8章 I6）。
// バイト数・行数・最長行長のいずれか超過で「大きすぎる」とみなす。
bool exceeds_limits(std::string_view content, const std::vector<std::string>& lines,
                    const DiffLimits& limits)
{
    if (content.size() > limits.max_total_bytes)
    {
        return true;
    }
    if (lines.size() > limits.max_lines)
    {
        return true;
    }
    for (const auto& line : lines)
    {
        if (line.size() > limits.max_line_bytes)
        {
            return true;
        }
    }
    return false;
}

// 相違量（編集距離 D の上限近似）が上限を超えるか（design.md 8章 I6 の補強）。
// dtl の O(N·D) は行数ガードを通っても D 由来で暴走する。同値行のオーバーラップを O(N) のハッシュで
// 数え、両側の「非共通行数」がともに max_diff_lines を超える＝共通部分が小さく D が大きい場合のみ
// 真を返す。全追加/全削除（片側の非共通行が0）や大ファイルの散在編集（共通行が多い）は軽いので通す。
bool exceeds_diff_distance(const std::vector<std::string>& old_lines,
                           const std::vector<std::string>& new_lines, const DiffLimits& limits)
{
    std::unordered_map<std::string_view, std::size_t> counts;
    counts.reserve(old_lines.size());
    for (const auto& l : old_lines)
    {
        ++counts[std::string_view(l)];
    }
    std::size_t common = 0;
    for (const auto& l : new_lines)
    {
        auto it = counts.find(std::string_view(l));
        if (it != counts.end() && it->second > 0)
        {
            --it->second;
            ++common;
        }
    }
    const std::size_t resid_old = old_lines.size() - common;
    const std::size_t resid_new = new_lines.size() - common;
    return std::min(resid_old, resid_new) > limits.max_diff_lines;
}

bool is_cancelled(const CancelTokenPtr& cancel)
{
    return cancel && cancel->is_cancelled();
}

// 連続する Delete 群と直後の Add 群を「変更ペア」とみなし、対応する行に行内強調を付ける。
// Delete[i] と Add[i] を位置対応させる（行差分の変更ブロックは置換ペアになることが多い）。
// 余った片側（行数が増減した分）は全体が新規/消滅なので強調を付けない（部分差が無い）。
void attach_inline_spans(std::vector<DiffLine>& lines, const CancelTokenPtr& cancel)
{
    std::size_t i = 0;
    while (i < lines.size())
    {
        if (lines[i].op != LineOp::Delete)
        {
            ++i;
            continue;
        }
        // 連続する Delete の範囲 [del_begin, del_end) を取る。
        const std::size_t del_begin = i;
        std::size_t del_end = i;
        while (del_end < lines.size() && lines[del_end].op == LineOp::Delete)
        {
            ++del_end;
        }
        // 直後に続く Add の範囲 [add_begin, add_end) を取る。
        const std::size_t add_begin = del_end;
        std::size_t add_end = add_begin;
        while (add_end < lines.size() && lines[add_end].op == LineOp::Add)
        {
            ++add_end;
        }

        const std::size_t del_count = del_end - del_begin;
        const std::size_t add_count = add_end - add_begin;
        const std::size_t pair_count = del_count < add_count ? del_count : add_count;
        for (std::size_t k = 0; k < pair_count; ++k)
        {
            if (is_cancelled(cancel))
            {
                return;
            }
            DiffLine& del = lines[del_begin + k];
            DiffLine& add = lines[add_begin + k];
            compute_inline_spans(del.text, add.text, del.spans, add.spans);
        }
        i = add_end > del_end ? add_end : del_end;
    }
}

} // namespace

DiffResult DiffEngine::compute(std::string_view old_content, std::string_view new_content,
                               const CancelTokenPtr& cancel) const
{
    DiffResult result;

    if (is_cancelled(cancel))
    {
        result.cancelled = true;
        return result;
    }

    std::vector<std::string> old_lines = split_lines_lf(old_content);
    std::vector<std::string> new_lines = split_lines_lf(new_content);

    // 開始前のサイズガード（要件8章「10MB以上自動オフ」。別スレッド中断に頼らない）。
    // サイズ（バイト/行数/最長行長）に加え、相違量（編集距離 D）でも開始前に弾く（dtl の O(N·D)
    // 暴走防止。exceeds_limits が false＝N<=max_lines のときだけ距離計算に進む＝O(N) が有界）。
    if (exceeds_limits(old_content, old_lines, limits_) ||
        exceeds_limits(new_content, new_lines, limits_) ||
        exceeds_diff_distance(old_lines, new_lines, limits_))
    {
        result.truncated = true;
        return result;
    }

    if (is_cancelled(cancel))
    {
        result.cancelled = true;
        return result;
    }

    result.lines = diff_lines(old_lines, new_lines);

    if (is_cancelled(cancel))
    {
        result.cancelled = true;
        return result;
    }

    attach_inline_spans(result.lines, cancel);
    if (is_cancelled(cancel))
    {
        result.cancelled = true;
        return result;
    }

    for (const auto& line : result.lines)
    {
        if (line.op == LineOp::Add)
        {
            ++result.added;
        }
        else if (line.op == LineOp::Delete)
        {
            ++result.removed;
        }
    }

    return result;
}

} // namespace pika::core::diff
