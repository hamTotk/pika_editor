#include "core/watcher/rename_tracker.h"

#include <algorithm>

namespace pika::core::watcher
{

namespace
{

bool within(TimeMs a, TimeMs b, TimeMs window)
{
    const TimeMs lo = a < b ? a : b;
    const TimeMs hi = a < b ? b : a;
    return (hi - lo) <= window;
}

FsEvent make_renamed(const std::string& old_path, const std::string& new_path, TimeMs at)
{
    FsEvent ev;
    ev.kind = FsEventKind::Renamed;
    ev.path = new_path;
    ev.old_path = old_path;
    ev.at = at;
    return ev;
}

FsEvent make_safe(const std::string& path, FsEventKind kind, TimeMs at)
{
    FsEvent ev;
    ev.kind = kind;
    ev.path = path;
    ev.at = at;
    return ev;
}

} // namespace

std::vector<FsEvent> RenameTracker::on_old(const std::string& old_path, TimeMs at)
{
    std::vector<FsEvent> out;
    // 窓内の未消費 NEW があればペア成立（NEW が先に届くケース）。最古から探す。
    for (auto it = pending_new_.begin(); it != pending_new_.end(); ++it)
    {
        if (within(it->at, at, pair_window_ms_))
        {
            out.push_back(make_renamed(old_path, it->path, std::max(it->at, at)));
            pending_new_.erase(it);
            return out;
        }
    }
    pending_old_.push_back(Half{old_path, at});
    return out;
}

std::vector<FsEvent> RenameTracker::on_new(const std::string& new_path, TimeMs at)
{
    std::vector<FsEvent> out;
    for (auto it = pending_old_.begin(); it != pending_old_.end(); ++it)
    {
        if (within(it->at, at, pair_window_ms_))
        {
            out.push_back(make_renamed(it->path, new_path, std::max(it->at, at)));
            pending_old_.erase(it);
            return out;
        }
    }
    pending_new_.push_back(Half{new_path, at});
    return out;
}

std::vector<FsEvent> RenameTracker::flush_expired(TimeMs now)
{
    std::vector<FsEvent> out;
    // 窓を超えても相方が来なかった OLD は削除扱い。
    for (auto it = pending_old_.begin(); it != pending_old_.end();)
    {
        if (now >= it->at && (now - it->at) > pair_window_ms_)
        {
            out.push_back(make_safe(it->path, FsEventKind::Removed, it->at));
            it = pending_old_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // 窓を超えても相方が来なかった NEW は新規扱い。
    for (auto it = pending_new_.begin(); it != pending_new_.end();)
    {
        if (now >= it->at && (now - it->at) > pair_window_ms_)
        {
            out.push_back(make_safe(it->path, FsEventKind::Created, it->at));
            it = pending_new_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    std::sort(out.begin(), out.end(),
              [](const FsEvent& a, const FsEvent& b) { return a.at < b.at; });
    return out;
}

std::vector<FsEvent> RenameTracker::flush_all()
{
    std::vector<FsEvent> out;
    for (const auto& h : pending_old_)
    {
        out.push_back(make_safe(h.path, FsEventKind::Removed, h.at));
    }
    for (const auto& h : pending_new_)
    {
        out.push_back(make_safe(h.path, FsEventKind::Created, h.at));
    }
    pending_old_.clear();
    pending_new_.clear();
    std::sort(out.begin(), out.end(),
              [](const FsEvent& a, const FsEvent& b) { return a.at < b.at; });
    return out;
}

} // namespace pika::core::watcher
