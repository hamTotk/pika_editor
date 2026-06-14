#include "core/watcher/event_synthesizer.h"

#include <algorithm>

namespace pika::core::watcher
{

void EventSynthesizer::push(const RawEvent& ev)
{
    auto& p = pending_[ev.path];
    p.last_at = std::max(p.last_at, ev.at);

    switch (ev.action)
    {
    case RawAction::Added:
    case RawAction::RenamedNew:
        p.saw_added = true;
        break;
    case RawAction::Removed:
    case RawAction::RenamedOld:
        p.saw_removed = true;
        break;
    case RawAction::Modified:
        break;
    }
    // 代表アクションは最後に届いたものを採るのではなく、合成結果の種別計算に必要な事実
    // （作成された・削除された・変更された）の集合を保持し、to_kind で解決する。
    p.effective = ev.action;
}

FsEventKind EventSynthesizer::to_kind(const Pending& p)
{
    // 同一パスの畳み込み内での種別解決（design.md 5.2 の安全側正規化に揃える）。
    // - 作成→…→削除 が同窓で揃ったら最終状態は「無い」だが、watcher は存在判定を持たないため
    //   最後の事実を優先する: 削除で終わるなら Removed、作成で終わるなら Created。
    // - それ以外（変更のみ・作成後に変更）は Modified に畳む（連続書き込み→1 イベント）。
    if (p.saw_removed && !p.saw_added)
    {
        return FsEventKind::Removed;
    }
    if (p.saw_added && !p.saw_removed)
    {
        return FsEventKind::Created;
    }
    if (p.saw_added && p.saw_removed)
    {
        // 同窓で作成と削除の両方を観測。最後のアクションで最終状態を決める（安全側）。
        if (p.effective == RawAction::Removed || p.effective == RawAction::RenamedOld)
        {
            return FsEventKind::Removed;
        }
        return FsEventKind::Created;
    }
    return FsEventKind::Modified;
}

std::vector<FsEvent> EventSynthesizer::drain(const std::vector<std::string>& paths)
{
    std::vector<FsEvent> out;
    out.reserve(paths.size());
    for (const auto& path : paths)
    {
        auto it = pending_.find(path);
        if (it == pending_.end())
        {
            continue;
        }
        FsEvent ev;
        ev.kind = to_kind(it->second);
        ev.path = path;
        ev.at = it->second.last_at;
        out.push_back(std::move(ev));
        pending_.erase(it);
    }
    // 確定時刻の昇順で返す（複数パス確定時の順序を決定論化する）。
    std::sort(out.begin(), out.end(),
              [](const FsEvent& a, const FsEvent& b) { return a.at < b.at; });
    return out;
}

std::vector<FsEvent> EventSynthesizer::flush(TimeMs now)
{
    std::vector<std::string> ready;
    for (const auto& [path, p] : pending_)
    {
        // 最後の生イベントからデバウンス窓ぶん静穏が続いたパスのみ確定する。
        // now < last_at の巻き戻りでは確定しない（安全側で次回へ持ち越す）。
        if (now >= p.last_at && (now - p.last_at) >= debounce_ms_)
        {
            ready.push_back(path);
        }
    }
    return drain(ready);
}

std::vector<FsEvent> EventSynthesizer::flush_all()
{
    std::vector<std::string> all;
    all.reserve(pending_.size());
    for (const auto& [path, p] : pending_)
    {
        (void)p;
        all.push_back(path);
    }
    return drain(all);
}

} // namespace pika::core::watcher
