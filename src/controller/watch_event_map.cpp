#include "controller/watch_event_map.h"

#include "core/watcher/resync.h"

namespace pika::controller
{

namespace wat = pika::core::watcher;

std::optional<wat::RawAction> map_watch_action(unsigned int action_code)
{
    switch (action_code)
    {
    case kActionAdded:
        return wat::RawAction::Added;
    case kActionRemoved:
        return wat::RawAction::Removed;
    case kActionModified:
        return wat::RawAction::Modified;
    case kActionRenamedOld:
        return wat::RawAction::RenamedOld;
    case kActionRenamedNew:
        return wat::RawAction::RenamedNew;
    default:
        // 未知コードは破棄する（誤った種別を未読/削除判定へ流さない）。
        return std::nullopt;
    }
}

std::string normalize_watch_rel_path(const std::string& raw_rel_utf8)
{
    std::string out;
    out.reserve(raw_rel_utf8.size());
    for (char c : raw_rel_utf8)
    {
        out.push_back(c == '\\' ? '/' : c);
    }
    // 先頭/末尾の余分な区切りを除く（ルート相対・先頭区切りなしの規約に合わせる）。
    std::size_t begin = 0;
    std::size_t end = out.size();
    while (begin < end && out[begin] == '/')
    {
        ++begin;
    }
    while (end > begin && out[end - 1] == '/')
    {
        --end;
    }
    return out.substr(begin, end - begin);
}

std::optional<wat::RawEvent> make_raw_event(unsigned int action_code, const std::string& rel_utf8,
                                            wat::TimeMs at)
{
    const std::optional<wat::RawAction> action = map_watch_action(action_code);
    if (!action.has_value())
    {
        return std::nullopt;
    }
    const std::string rel = normalize_watch_rel_path(rel_utf8);
    if (rel.empty())
    {
        // ルート自身・空パスは監視対象ファイルではない（呼び出し側は投入しない）。
        return std::nullopt;
    }
    if (wat::is_pika_temp_file(rel))
    {
        // pika 自身のアトミック書き込み一時ファイルは未読/差分に流さない（F-014「幽霊未読」対策）。
        return std::nullopt;
    }
    wat::RawEvent ev;
    ev.action = action.value();
    ev.path = rel;
    ev.at = at;
    return ev;
}

} // namespace pika::controller
