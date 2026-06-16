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

    for (std::size_t i = 0; i < events.size(); ++i)
    {
        const wat::FsEvent& ev = events[i];

        // Renamed は連続する run を 1 回の apply_renames へまとめて適用する。
        // apply_renames の往復検出（A→B→A）は呼び出し内 seen_pairs で行うため、イベントを
        // 1 件ずつ別呼び出しにすると対応付け不能ケースを取りこぼす（要件4.2）。
        // 連続 run だけをまとめることで Created/Modified/Removed との順序関係は崩さない。
        if (ev.kind == wat::FsEventKind::Renamed)
        {
            std::size_t end = i;
            while (end < events.size() && events[end].kind == wat::FsEventKind::Renamed)
            {
                ++end;
            }
            apply_rename_run(events, i, end, changes);
            i = end - 1; // 次のループで end の位置へ進む
            continue;
        }

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
        case wat::FsEventKind::Renamed:
            // Renamed はループ先頭で run 単位に処理済み（ここには来ない）。
            break;
        }
    }
    return changes;
}

void WorkspaceController::apply_rename_run(const std::vector<wat::FsEvent>& events,
                                           std::size_t begin, std::size_t end,
                                           std::vector<FsChange>& changes)
{
    // rename/移動。core/workspace::apply_renames で未読・ベースライン・退避を新パスへ引き継ぐ
    // （relPath 付け替え。design.md 5.2・要件4.2/7.2）。連続 run を 1 回でまとめて適用すると、
    // 往復（A→B→A）が呼び出し内の往復検出に乗り reevaluate/orphaned が正しく返る。
    std::vector<ws::RenameOp> ops;
    ops.reserve(end - begin);
    std::vector<bool> old_was_unread;
    old_was_unread.reserve(end - begin);
    for (std::size_t k = begin; k < end; ++k)
    {
        ws::RenameOp op;
        op.old_path = events[k].old_path;
        op.new_path = events[k].path;
        ops.push_back(op);
        old_was_unread.push_back(unread_.is_unread(events[k].old_path));
    }

    const ws::CarryResult res = ws::apply_renames(states_, ops);
    states_ = res.states;

    // 先に未読集合の旧→新移動と各イベントの反映結果を作る。reevaluate 由来の未読化を後段で
    // 行うのは、この移動ループの clear(old_path) が往復の終端パス（再判定対象）を消し戻すのを
    // 避けるため。
    for (std::size_t k = begin; k < end; ++k)
    {
        const wat::FsEvent& ev = events[k];
        unread_.clear(ev.old_path);
        if (old_was_unread[k - begin])
        {
            unread_.mark(ev.path);
        }
        FsChange c;
        c.effect = FsChangeEffect::RenamedCarried;
        c.path = ev.path;
        c.old_path = ev.old_path;
        // この run で生じた reevaluate/orphaned を run 内の各 rename イベントへ載せる
        // （イベント単位の厳密な帰属は core が返さないため run の結果として透過する）。
        c.reevaluate = res.reevaluate;
        c.orphaned = res.orphaned;
        changes.push_back(std::move(c));
    }

    // reevaluate＝対応付け不能で最終ディスク内容での再判定が必要なパス。controller 側では当該
    // エントリのベースラインを暫定無効化して「再差分対象」に倒す（安全側。確定読みで再判定される
    // まで Modified を ± ではなく新規 ◆ 相当に扱い、誤った差分基準で見せない）。
    for (const auto& rel : res.reevaluate)
    {
        ws::CarryState& st = states_[rel];
        st.has_baseline = false;
        st.baseline_hash = 0;
        st.unread = true;
        unread_.mark(rel);
        reevaluate_.push_back(rel);
    }

    // orphaned＝引き継ぎ失敗で旧キーが孤立保全されたパス。握り潰さず累積し UI/ログへ流す。
    for (const auto& rel : res.orphaned)
    {
        orphaned_.push_back(rel);
    }
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
