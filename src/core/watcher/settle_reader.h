// core/watcher: 確定読み（中途内容の防止）。
// design.md 5.2「確定読み（中途内容の防止）」。要件7章。sprint3 must「確定読み」。
//
// 直近変更からデバウンス分の静穏期間を待ち、かつ mtime/サイズが連続観測間で安定したことを
// 確認してから内容を確定読みする（共有モードで書く実装の末尾欠損を防ぐ）。本クラスは「いつ確定読み
// してよいか」だけを純ロジックで判定する（実際の読み取りは呼び出し側＝fs_probe が行う）。
//
// 使い方: 変更を観測するたび observe(stat, now) を呼び、ready_to_read(now) が true になってから
// 内容を読む。観測のたびに size/mtime
// が変われば安定カウントがリセットされ、確定はさらに待たされる。
#pragma once

#include "core/watcher/fs_event.h"
#include "core/watcher/fs_probe.h"

#include <cstdint>

namespace pika::core::watcher
{

class SettleReader
{
  public:
    // 静穏期間（ms）。最後の変化からこれだけ無変化が続いて初めて確定可。
    static constexpr TimeMs kDefaultQuietMs = 100;
    // 安定確認に必要な「同一 size/mtime」連続観測回数（最低 2 回＝1 回ぶんの安定が取れる）。
    static constexpr int kDefaultStableObservations = 2;

    explicit SettleReader(TimeMs quiet_ms = kDefaultQuietMs,
                          int stable_observations = kDefaultStableObservations)
        : quiet_ms_(quiet_ms), stable_needed_(stable_observations < 2 ? 2 : stable_observations)
    {
    }

    // 変更通知の都度、現在の stat と時刻を渡す。
    // size/mtime
    // が前回と同じなら安定カウントを増やし、変われば最後の変化時刻を更新してリセットする。
    void observe(const FileStat& st, TimeMs now);

    // now 時点で確定読みしてよいか。
    // 条件: 1 回以上 observe 済み・存在する・連続安定が stable_needed_ 回以上・
    //       最後の変化から quiet_ms_ 経過。中途内容（書き込み継続中）では false を返す。
    bool ready_to_read(TimeMs now) const;

    // 確定読みを 1 回成立させた後にリセットする（次の変更サイクルへ）。
    void reset();

    int stable_count() const noexcept { return stable_count_; }

  private:
    bool seen_ = false;
    bool exists_ = false;
    std::uint64_t last_size_ = 0;
    std::uint64_t last_mtime_ns_ = 0;
    TimeMs last_change_at_ = 0; // size/mtime が最後に変化した時刻
    int stable_count_ = 0;      // 同一 size/mtime の連続観測回数
    TimeMs quiet_ms_;
    int stable_needed_;
};

} // namespace pika::core::watcher
