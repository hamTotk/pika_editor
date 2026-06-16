#include "controller/workspace_controller.h"

#include <algorithm>
#include <utility>

namespace pika::controller
{

namespace ws = pika::core::workspace;
namespace wat = pika::core::watcher;

WorkspaceController::WorkspaceController(std::string root) : root_(std::move(root)) {}

void WorkspaceController::set_baseline(const wat::BaselineMap& baseline)
{
    // 起動時/再同期後のベースラインを取り込む。ここに載るファイルは「ベースラインあり」＝
    // 以後の Modified は ±（Diff）として表示し、Created（baseline 不在の新規）は ◆ として弁別する。
    baseline_ = baseline; // resync の突き合わせ基準として保持する。
    for (const auto& [rel, entry] : baseline)
    {
        ws::CarryState& st = states_[rel];
        st.has_baseline = true;
        st.baseline_hash = entry.hash_lf;
    }
}

void WorkspaceController::touch_unread(const std::string& rel_path, bool has_baseline)
{
    ws::CarryState& st = states_[rel_path];
    st.unread = true;
    st.has_baseline = has_baseline;
    unread_.mark(rel_path);
}

std::vector<FsChange> WorkspaceController::apply_events(const std::vector<wat::FsEvent>& events)
{
    std::vector<FsChange> changes;
    changes.reserve(events.size());

    for (const auto& ev : events)
    {
        switch (ev.kind)
        {
        case wat::FsEventKind::Created: {
            // 新規作成。ベースラインを持たない未読＝新規（◆）として表示する（要件4.2）。
            // 既にベースラインを持つパスへの「作成」（削除→再作成等）はその有無を尊重する。
            auto it = states_.find(ev.path);
            const bool has_baseline = it != states_.end() && it->second.has_baseline;
            touch_unread(ev.path, has_baseline);
            FsChange c;
            c.effect = FsChangeEffect::UnreadMarked;
            c.path = ev.path;
            c.is_new = !has_baseline;
            changes.push_back(std::move(c));
            break;
        }
        case wat::FsEventKind::Modified: {
            // 内容変更。ベースラインがあれば ±（Diff）、無ければ新規扱い（◆）に倒す
            // （起動前にベースライン化されていない＝まだ確認されていないファイル）。
            auto it = states_.find(ev.path);
            const bool has_baseline = it != states_.end() && it->second.has_baseline;
            touch_unread(ev.path, has_baseline);
            FsChange c;
            c.effect = FsChangeEffect::UnreadMarked;
            c.path = ev.path;
            c.is_new = !has_baseline;
            changes.push_back(std::move(c));
            break;
        }
        case wat::FsEventKind::Removed: {
            // 外部削除。ツリーからは消えるため未読集合から外す（伝播未読も自然に解消）。
            // ただし引き継ぎ状態（退避 ID・ベースライン）は孤立保全で残す（消失タブの安全遷移・
            // 90日GC に委ねる。design.md 5.1 手順4・要件4.2/7.2）。タブ側は削除済み表示へ遷移する。
            unread_.clear(ev.path);
            FsChange c;
            c.effect = FsChangeEffect::PathRemoved;
            c.path = ev.path;
            changes.push_back(std::move(c));
            break;
        }
        case wat::FsEventKind::Renamed: {
            // rename/移動。core/workspace::apply_renames で未読・ベースライン・退避を新パスへ
            // 引き継ぐ（relPath 付け替え。design.md 5.2・要件4.2/7.2）。未読集合も旧→新へ移す。
            const bool old_was_unread = unread_.is_unread(ev.old_path);

            ws::RenameOp op;
            op.old_path = ev.old_path;
            op.new_path = ev.path;
            const ws::CarryResult res = ws::apply_renames(states_, {op});
            states_ = res.states;

            unread_.clear(ev.old_path);
            if (old_was_unread)
            {
                unread_.mark(ev.path);
            }
            FsChange c;
            c.effect = FsChangeEffect::RenamedCarried;
            c.path = ev.path;
            c.old_path = ev.old_path;
            changes.push_back(std::move(c));
            break;
        }
        }
    }
    return changes;
}

void WorkspaceController::mark_confirmed(const std::string& rel_path)
{
    unread_.clear(rel_path);
    auto it = states_.find(rel_path);
    if (it != states_.end())
    {
        it->second.unread = false;
    }
}

std::vector<std::string> WorkspaceController::new_files() const
{
    // ベースラインを持たない未読ファイル＝新規（◆）。build_tree_view_model の new_files へ渡す。
    std::vector<std::string> out;
    for (const auto& rel : unread_.items())
    {
        auto it = states_.find(rel);
        const bool has_baseline = it != states_.end() && it->second.has_baseline;
        if (!has_baseline)
        {
            out.push_back(rel);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

TreeRowVm WorkspaceController::build_view_model(const ws::TreeNode& tree) const
{
    return build_tree_view_model(tree, unread_, new_files());
}

} // namespace pika::controller
