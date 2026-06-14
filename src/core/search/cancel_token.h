// core/search: 協調キャンセル用のアトミックフラグ。
// design.md 3章 core/search「巨大/長行入力はワーカーで実行しキャンセル可」。要件5.4「巨大ファイル・
// 長行での検索・全置換は…キャンセル可能な処理として進捗を表示する」。sprint9 must「巨大/長行入力の
// 検索・全置換がキャンセル可能な処理として実装され、協調キャンセルで中断できる」。
//
// PCRE2 の 1 マッチ呼び出しは内部にキャンセル点を持たないため、検索/全置換の「ヒットごとの反復」
// という pika 側で刻める区切りで協調的に観測する（別スレッド中断には頼らない）。
// core/diff の CancelToken と同型だが、検索は別モジュール・別 namespace のため独自に持つ
// （モジュール間の依存を増やさない＝コアサービス同士は原則独立。CLAUDE.md アーキテクチャ）。
#pragma once

#include <atomic>
#include <memory>

namespace pika::core::search
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

} // namespace pika::core::search
