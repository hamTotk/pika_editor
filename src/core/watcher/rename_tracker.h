// core/watcher: rename の正規化（old/new ペアの突き合わせと安全側フォールバック）。
// design.md 5.2「アトミック置換の検知」/ 13章「rename正規化・ペア不成立」。
// 要件7章。sprint3 must「rename 正規化」。
//
// `ReadDirectoryChangesW` の RENAMED_OLD_NAME / RENAMED_NEW_NAME は別々のイベントとして届く。
// 時間窓内に old/new が揃えば Renamed（移動/改名）として追従し、揃わない場合は安全側に倒す:
//   - OLD 単独（new が来ない）   → Removed（削除扱い）
//   - NEW 単独（old が来ない）   → Created（新規扱い。上書き先に既存があっても内容変更として処理）
//   - 既存ファイルへの上書き rename → 対象パスの内容変更（呼び出し側が存在判定して Modified
//   に倒す）
//
// 純ロジック。OS 通知の到着順・時刻を注入して gtest で決定論検証する。
#pragma once

#include "core/watcher/fs_event.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pika::core::watcher
{

class RenameTracker
{
  public:
    // old/new を 1 つの rename と見なすペア突き合わせ窓（ms）。
    static constexpr TimeMs kDefaultPairWindowMs = 200;

    explicit RenameTracker(TimeMs pair_window_ms = kDefaultPairWindowMs)
        : pair_window_ms_(pair_window_ms)
    {
    }

    // RENAMED_OLD_NAME を取り込む。直前に未消費の NEW が窓内にあれば即ペア成立させ FsEvent を返す。
    // 揃わなければ保留し、空の戻り値（呼び出し側は flush_expired で後から確定する）。
    std::vector<FsEvent> on_old(const std::string& old_path, TimeMs at);

    // RENAMED_NEW_NAME を取り込む。直前に未消費の OLD が窓内にあればペア成立。
    std::vector<FsEvent> on_new(const std::string& new_path, TimeMs at);

    // now 時点で窓を超えても相方が来なかった保留を安全側に確定する
    // （OLD 単独→Removed、NEW 単独→Created）。確定した保留は破棄する。
    std::vector<FsEvent> flush_expired(TimeMs now);

    // 全保留を安全側に確定する（オーバーフロー再同期前のドレイン等）。
    std::vector<FsEvent> flush_all();

    bool empty() const noexcept { return pending_old_.empty() && pending_new_.empty(); }

  private:
    struct Half
    {
        std::string path;
        TimeMs at = 0;
    };

    // 窓内に相方がない単独の OLD / NEW を保留する（到着順）。
    std::vector<Half> pending_old_;
    std::vector<Half> pending_new_;
    TimeMs pair_window_ms_;
};

} // namespace pika::core::watcher
