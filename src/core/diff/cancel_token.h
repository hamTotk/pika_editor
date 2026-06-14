// core/diff: 協調キャンセル用のアトミックフラグ。
// design.md 4章「差分計算・プレビュー変換・検索/全置換は…古いタスクは協調キャンセル
// （アトミックフラグ）」。sprint6 should「差分計算がキャンセル可能（協調キャンセルのアトミック
// フラグ）である」。
//
// dtl（Myers）/ md4c は 1 回の呼び出しがブロッキングで内部にキャンセル点を持たないため、
// キャンセルは「行内強調の反復」「行の走査」など pika 側で刻める区切りで協調的に観測する
// （別スレッド中断には頼らない）。共有して参照する想定のため shared_ptr で持ち回る。
#pragma once

#include <atomic>
#include <memory>

namespace pika::core::diff
{

class CancelToken
{
  public:
    void cancel() noexcept { flag_.store(true, std::memory_order_relaxed); }
    bool is_cancelled() const noexcept { return flag_.load(std::memory_order_relaxed); }

  private:
    std::atomic<bool> flag_{false};
};

using CancelTokenPtr = std::shared_ptr<CancelToken>;

} // namespace pika::core::diff
