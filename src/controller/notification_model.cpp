#include "controller/notification_model.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

namespace pika::controller
{

NotificationView aggregate_notifications(const std::vector<Notification>& notifications,
                                         const std::string& active_tab_path)
{
    NotificationView view;

    // 1)
    // アクティブタブ文脈で対象を絞る（タブ固有はアクティブタブのものだけ＋グローバルは常に対象）。
    //    同一ファイル・同一種別は最新（seq 最大）の 1 件へ集約する（キー＝(kind, tab_path)）。
    //    グローバルは tab_path が空＝(kind, "") をキーに同種最新へ集約する。
    std::map<std::pair<NotificationKind, std::string>, Notification> latest;
    for (const auto& n : notifications)
    {
        const bool is_global = n.tab_path.empty();
        const bool matches_active = !is_global && n.tab_path == active_tab_path;
        if (!is_global && !matches_active)
        {
            continue; // 他タブ固有の通知は出さない（タブ固有/グローバルの切替）。
        }

        const std::pair<NotificationKind, std::string> key{n.kind, n.tab_path};
        auto it = latest.find(key);
        if (it == latest.end() || n.seq > it->second.seq)
        {
            latest[key] = n; // 同一ファイル・同一種別の最新（seq 最大）へ集約。
        }
    }

    // 2) 集約後の通知を優先順位（kind の列挙順＝小さいほど高優先）→同種は seq 降順で整列する。
    std::vector<Notification> ordered;
    ordered.reserve(latest.size());
    for (auto& [key, n] : latest)
    {
        ordered.push_back(std::move(n));
    }
    std::sort(ordered.begin(), ordered.end(), [](const Notification& a, const Notification& b) {
        if (a.kind != b.kind)
        {
            return static_cast<int>(a.kind) < static_cast<int>(b.kind);
        }
        // 同種別は新しいもの（seq 大）を上に。
        return a.seq > b.seq;
    });

    // 3) 先頭 kMaxVisible 本を表示行へ、超過分の件数を overflow（「他N件」）へ。
    const std::size_t visible = std::min(ordered.size(), kMaxVisible);
    for (std::size_t i = 0; i < visible; ++i)
    {
        const Notification& n = ordered[i];
        NotificationRow row;
        row.kind = n.kind;
        row.tab_path = n.tab_path;
        row.seq = n.seq;
        row.detail = n.detail;
        row.is_global = n.tab_path.empty();
        view.rows.push_back(std::move(row));
    }
    view.overflow = ordered.size() - visible;

    return view;
}

} // namespace pika::controller
