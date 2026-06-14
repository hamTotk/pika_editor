#include "core/watcher/watcher_core.h"

#include <algorithm>
#include <utility>

namespace pika::core::watcher
{

WatcherCore::WatcherCore(HashProbe hash_probe, TimeMs debounce_ms, TimeMs rename_window_ms)
    : hash_probe_(std::move(hash_probe)), synth_(debounce_ms), renames_(rename_window_ms)
{
}

void WatcherCore::on_raw(const RawEvent& ev)
{
    // rename の old/new は専用トラッカへ。揃えば即 Renamed が確定するが、ここでは溜めるだけにし、
    // poll で他イベントと時刻順に揃えて返す（順序の決定論を poll に集約する）。
    switch (ev.action)
    {
    case RawAction::RenamedOld: {
        // 窓内に相方 NEW があれば即ペア成立（Renamed）。poll まで保持する。
        auto paired = renames_.on_old(ev.path, ev.at);
        for (auto& p : paired)
        {
            pending_renamed_.push_back(std::move(p));
        }
        break;
    }
    case RawAction::RenamedNew: {
        auto paired = renames_.on_new(ev.path, ev.at);
        for (auto& p : paired)
        {
            pending_renamed_.push_back(std::move(p));
        }
        break;
    }
    default:
        synth_.push(ev);
        break;
    }
}

void WatcherCore::register_self_save(const std::string& path, std::uint64_t hash_lf, TimeMs at)
{
    self_save_.register_save(path, hash_lf, at);
}

bool WatcherCore::suppressed_as_self_save(const FsEvent& ev, TimeMs now)
{
    // 自己保存抑制は「内容がディスク上で保存後ハッシュに一致する」ことを主条件とする。
    // 削除・rename は内容ハッシュで判定しないため対象外（外部削除/移動を握り潰さない）。
    if (ev.kind != FsEventKind::Created && ev.kind != FsEventKind::Modified)
    {
        return false;
    }
    const std::optional<std::uint64_t> disk_hash =
        hash_probe_ ? hash_probe_(ev.path) : std::nullopt;
    if (!disk_hash.has_value())
    {
        return false; // 読めない/不在は外部変更として扱う（design.md 5.2）
    }
    return self_save_.consume_if_self(ev.path, *disk_hash, now);
}

std::vector<FsEvent> WatcherCore::poll(TimeMs now)
{
    std::vector<FsEvent> events;

    // 1a) on_raw でペア成立した Renamed（pending_renamed_ に溜めた分）を出す。
    for (auto& ev : pending_renamed_)
    {
        events.push_back(std::move(ev));
    }
    pending_renamed_.clear();

    // 1b) 窓を超えても相方が来なかった単独 rename を安全側（Removed/Created）へ確定する。
    auto expired = renames_.flush_expired(now);
    for (auto& ev : expired)
    {
        events.push_back(std::move(ev));
    }

    // 2) デバウンス窓を越えた合成イベント。
    auto synthesized = synth_.flush(now);
    for (auto& ev : synthesized)
    {
        // 3) 自己保存抑制（Created/Modified のみ）。一致すれば落とす。
        if (suppressed_as_self_save(ev, now))
        {
            continue;
        }
        events.push_back(std::move(ev));
    }

    // 補助 GC（滞留トークンの破棄）。主条件はハッシュ一致なので順序非依存。
    self_save_.gc(now);

    // 時刻昇順で決定論化する（rename と合成が混ざるため）。
    std::sort(events.begin(), events.end(),
              [](const FsEvent& a, const FsEvent& b) { return a.at < b.at; });
    return events;
}

void WatcherCore::drain_for_resync()
{
    // 保留中の部分イベントを破棄する（再列挙が全状態を再構成するため二重計上を防ぐ）。
    synth_.flush_all();
    renames_.flush_all();
    pending_renamed_.clear();
}

} // namespace pika::core::watcher
