// core/watcher: イベント合成（デバウンス＋連続書き込みの 1 イベント化）。
// design.md 5.2「デバウンス100ms・イベント合成」/ 13章「連続書き込み→1回」。
// 要件7章。sprint3 must「イベント合成」。
//
// 短時間（既定100ms）に同一パスへ届く複数の生イベントを 1 つの FsEvent に畳む。畳む際は
// 種別の優先順位（rename > 削除 > 作成 > 変更）で正規化し、最後のイベント時刻を確定時刻とする。
// rename の old/new ペア突き合わせは RenameTracker
// に委譲する（本クラスは同一パスの畳み込みに専念）。
//
// 純ロジック（OS 通知の代わりに RawEvent を push し、時刻を渡して flush する）。gtest
// で決定論検証。
#pragma once

#include "core/watcher/fs_event.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pika::core::watcher
{

class EventSynthesizer
{
  public:
    static constexpr TimeMs kDefaultDebounceMs = 100;

    explicit EventSynthesizer(TimeMs debounce_ms = kDefaultDebounceMs) : debounce_ms_(debounce_ms)
    {
    }

    // 生イベントを 1 件取り込む（合成バッファに溜める。まだ FsEvent は出さない）。
    void push(const RawEvent& ev);

    // now 時点でデバウンス窓（debounce_ms_）を経過した（＝最後の生イベントから静穏が続いた）
    // パスを確定し、合成済み FsEvent 列を返す。確定したパスはバッファから除く。
    // 窓内のパスは保留（次回 flush へ持ち越し）。確定順は最後の生イベント時刻の昇順。
    std::vector<FsEvent> flush(TimeMs now);

    // 強制フラッシュ（オーバーフロー再同期前のドレイン等）。窓を待たず全バッファを確定する。
    std::vector<FsEvent> flush_all();

    bool empty() const noexcept { return pending_.empty(); }

  private:
    // 同一パスに対する畳み込み中の状態。
    struct Pending
    {
        RawAction effective = RawAction::Modified; // 優先順位で正規化した代表アクション
        TimeMs last_at = 0;                        // 最後の生イベント時刻（静穏判定・確定時刻）
        bool saw_added = false;
        bool saw_removed = false;
    };

    std::vector<FsEvent> drain(const std::vector<std::string>& paths);
    static FsEventKind to_kind(const Pending& p);

    std::unordered_map<std::string, Pending> pending_;
    TimeMs debounce_ms_;
};

} // namespace pika::core::watcher
