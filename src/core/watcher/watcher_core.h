// core/watcher: UI 非依存の監視コアパイプライン。
// design.md 3章「`FileWatcher`, `FsEvent`」/ 5.2。要件7章。
//
// プラットフォーム層（`ReadDirectoryChangesW` を所有する監視スレッド）が供給する生イベントを、
// イベント合成 → rename 正規化 → 自己保存抑制 の順に通し、WorkspaceController へ渡す確定 FsEvent
// 列を生成する。実 FS 読み取り（確定読み・オーバーフロー再同期）は本クラスの外（fs_probe/resync）。
//
// 本クラスは wx・Win32 を含まず、時刻と生イベントを注入して gtest で決定論検証できる。
// 自己保存判定に必要な「現ディスク内容のハッシュ」は注入関数（HashProbe）で渡す（テスト容易性）。
#pragma once

#include "core/watcher/event_synthesizer.h"
#include "core/watcher/fs_event.h"
#include "core/watcher/rename_tracker.h"
#include "core/watcher/self_save_guard.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pika::core::watcher
{

class WatcherCore
{
  public:
    // path の現ディスク内容の LF 正規化ハッシュを返す関数。存在しない/読めない場合は 0 を返す約束。
    // 実機では fs_probe::content_hash_lf を束ねる。テストはディスクのモックを返す。
    using HashProbe = std::function<std::uint64_t(const std::string& path)>;

    WatcherCore(HashProbe hash_probe, TimeMs debounce_ms = EventSynthesizer::kDefaultDebounceMs,
                TimeMs rename_window_ms = RenameTracker::kDefaultPairWindowMs);

    // 監視スレッドが受けた生イベントを 1 件投入する（まだ確定しない）。
    void on_raw(const RawEvent& ev);

    // 保存直前に登録する自己保存トークン（パス＋保存後 LF ハッシュ）。
    void register_self_save(const std::string& path, std::uint64_t hash_lf, TimeMs at);

    // now 時点で確定した FsEvent 列を返す。
    // 1) rename の期限切れ単独を安全側へ確定 2) デバウンス窓を越えたパスを合成
    // 3) Created/Modified
    // は自己保存トークンと突き合わせ、ディスクハッシュ一致なら抑制（消費）する。
    std::vector<FsEvent> poll(TimeMs now);

    // オーバーフロー再同期の前後に呼ぶ: 保留中の合成・rename を全てドレインして捨てる
    // （再列挙が全状態を再構成するため、途中の部分イベントは破棄して二重計上を防ぐ）。
    void drain_for_resync();

  private:
    // 自己保存抑制を適用する（Created/Modified のみ対象。rename/削除は内容ハッシュで判定しない）。
    bool suppressed_as_self_save(const FsEvent& ev, TimeMs now);

    HashProbe hash_probe_;
    EventSynthesizer synth_;
    RenameTracker renames_;
    SelfSaveGuard self_save_;
    // on_raw でペア成立した Renamed を poll まで保持する（成立ペアを取りこぼさない）。
    std::vector<FsEvent> pending_renamed_;
};

} // namespace pika::core::watcher
