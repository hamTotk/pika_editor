// controller/workspace_controller の検証（sprint4 must）。
// - FsEvent（Created/Modified/Removed/Renamed）を受けて UnreadSet・引き継ぎ状態を更新する。
// - 自己保存トークン照合（WatcherCore::poll で消し込み）→ 自己保存は未読化せず外部変更は未読化。
// - rename 追従：apply_renames で未読・ベースラインを新パスへ引き継ぐ（relPath 付け替え）。
// - 削除は消失タブ安全遷移（PathRemoved）＋状態の孤立保全。
// - 新規（◆・ベースラインなし）と差分あり（±・ベースラインあり）を区別できる。
#include "controller/tree_view_model.h"
#include "controller/workspace_controller.h"
#include "core/watcher/fs_event.h"
#include "core/watcher/resync.h"
#include "core/watcher/watcher_core.h"
#include "core/workspace/workspace_model.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace
{

using pika::controller::FsChange;
using pika::controller::FsChangeEffect;
using pika::controller::StateMark;
using pika::controller::TreeRowVm;
using pika::controller::WorkspaceController;
using pika::core::watcher::BaselineEntry;
using pika::core::watcher::BaselineMap;
using pika::core::watcher::FsEvent;
using pika::core::watcher::FsEventKind;
using pika::core::watcher::RawAction;
using pika::core::watcher::RawEvent;
using pika::core::watcher::WatcherCore;

FsEvent make_event(FsEventKind kind, const std::string& path, const std::string& old_path = {})
{
    FsEvent ev;
    ev.kind = kind;
    ev.path = path;
    ev.old_path = old_path;
    return ev;
}

// ---- Created / Modified → 未読化 ----

TEST(WorkspaceControllerTest, CreatedMarksUnreadAsNew)
{
    WorkspaceController wc("C:/ws");
    const auto changes = wc.apply_events({make_event(FsEventKind::Created, "new.md")});

    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].effect, FsChangeEffect::UnreadMarked);
    EXPECT_EQ(changes[0].path, "new.md");
    EXPECT_TRUE(changes[0].is_new); // ベースラインなし＝新規（◆）
    EXPECT_TRUE(wc.unread().is_unread("new.md"));
}

TEST(WorkspaceControllerTest, ModifiedWithBaselineMarksUnreadAsDiff)
{
    WorkspaceController wc("C:/ws");
    BaselineMap base;
    base["doc.md"] = BaselineEntry{10, 100, 0xABCD};
    wc.set_baseline(base);

    const auto changes = wc.apply_events({make_event(FsEventKind::Modified, "doc.md")});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].effect, FsChangeEffect::UnreadMarked);
    EXPECT_FALSE(changes[0].is_new); // ベースラインあり＝差分あり（±）、新規ではない
    EXPECT_TRUE(wc.unread().is_unread("doc.md"));
}

TEST(WorkspaceControllerTest, ModifiedWithoutBaselineFallsBackToNew)
{
    // ベースライン化前のファイルへの Modified は新規（◆）に倒す（まだ確認されていない）。
    WorkspaceController wc("C:/ws");
    const auto changes = wc.apply_events({make_event(FsEventKind::Modified, "fresh.md")});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_TRUE(changes[0].is_new);
}

// ---- 削除は消失タブ安全遷移（PathRemoved）＋未読解除・状態保全 ----

TEST(WorkspaceControllerTest, RemovedReportsPathRemovedAndClearsUnread)
{
    WorkspaceController wc("C:/ws");
    BaselineMap base;
    base["gone.md"] = BaselineEntry{5, 50, 0x1111};
    wc.set_baseline(base);
    wc.apply_events({make_event(FsEventKind::Modified, "gone.md")});
    ASSERT_TRUE(wc.unread().is_unread("gone.md"));

    const auto changes = wc.apply_events({make_event(FsEventKind::Removed, "gone.md")});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].effect, FsChangeEffect::PathRemoved);
    EXPECT_EQ(changes[0].path, "gone.md");
    // ツリーから消えるので未読集合からは外れる。
    EXPECT_FALSE(wc.unread().is_unread("gone.md"));
    // ただし引き継ぎ状態（退避・ベースライン）は孤立保全で残す（消失タブの安全遷移）。
    EXPECT_NE(wc.states().find("gone.md"), wc.states().end());
}

// ---- rename 追従：未読・ベースラインを新パスへ引き継ぐ ----

TEST(WorkspaceControllerTest, RenameCarriesUnreadToNewPath)
{
    WorkspaceController wc("C:/ws");
    BaselineMap base;
    base["old.md"] = BaselineEntry{8, 80, 0x2222};
    wc.set_baseline(base);
    wc.apply_events({make_event(FsEventKind::Modified, "old.md")});
    ASSERT_TRUE(wc.unread().is_unread("old.md"));

    const auto changes =
        wc.apply_events({make_event(FsEventKind::Renamed, "renamed.md", "old.md")});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].effect, FsChangeEffect::RenamedCarried);
    EXPECT_EQ(changes[0].path, "renamed.md");
    EXPECT_EQ(changes[0].old_path, "old.md");

    // 未読は旧→新へ移る（要件4.2「未読を引き継ぐ」）。
    EXPECT_FALSE(wc.unread().is_unread("old.md"));
    EXPECT_TRUE(wc.unread().is_unread("renamed.md"));

    // ベースライン（CarryState）も新パスへ移っている（旧パスは消える）。
    EXPECT_EQ(wc.states().find("old.md"), wc.states().end());
    auto it = wc.states().find("renamed.md");
    ASSERT_NE(it, wc.states().end());
    EXPECT_TRUE(it->second.has_baseline);
    EXPECT_EQ(it->second.baseline_hash, 0x2222u);
}

TEST(WorkspaceControllerTest, RenameCarriesStashIds)
{
    // 退避 ID（stash_ids）の引き継ぎ（要件7.2「退避を引き継ぐ」）。
    WorkspaceController wc("C:/ws");
    BaselineMap base;
    base["a.md"] = BaselineEntry{1, 1, 0x9};
    wc.set_baseline(base);
    // 退避結合は DocumentController（sprint6）。ここでは rename での引き継ぎ経路だけを観測する。
    // rename 前に Modified で状態を作り、引き継ぎ後にベースラインが新キーへ移ることを見る。
    wc.apply_events({make_event(FsEventKind::Modified, "a.md")});
    const auto changes = wc.apply_events({make_event(FsEventKind::Renamed, "b.md", "a.md")});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].effect, FsChangeEffect::RenamedCarried);
    // ベースラインが新キーへ引き継がれている（relPath 付け替え）。
    auto it = wc.states().find("b.md");
    ASSERT_NE(it, wc.states().end());
    EXPECT_EQ(it->second.baseline_hash, 0x9u);
}

// ---- 確認済み（未読解除）。確認済みフロー本体は sprint6、ここでは未読除去のみ観測 ----

TEST(WorkspaceControllerTest, MarkConfirmedClearsUnread)
{
    WorkspaceController wc("C:/ws");
    wc.apply_events({make_event(FsEventKind::Created, "x.md")});
    ASSERT_TRUE(wc.unread().is_unread("x.md"));
    wc.mark_confirmed("x.md");
    EXPECT_FALSE(wc.unread().is_unread("x.md"));
}

// ---- ベースライン保持（resync 突き合わせ基準） ----

TEST(WorkspaceControllerTest, BaselineIsRetainedForResync)
{
    WorkspaceController wc("C:/ws");
    BaselineMap base;
    base["a.md"] = BaselineEntry{3, 30, 0x7};
    base["b.md"] = BaselineEntry{4, 40, 0x8};
    wc.set_baseline(base);

    // set_baseline で取り込んだメタが resync の突き合わせ基準として取り出せる。
    const auto& kept = wc.baseline();
    ASSERT_EQ(kept.size(), 2u);
    auto it = kept.find("a.md");
    ASSERT_NE(it, kept.end());
    EXPECT_EQ(it->second.size, 3u);
    EXPECT_EQ(it->second.hash_lf, 0x7u);
}

// ---- new_files()：新規（◆）と差分あり（±）の弁別 ----

TEST(WorkspaceControllerTest, NewFilesListsOnlyBaselinelessUnread)
{
    WorkspaceController wc("C:/ws");
    BaselineMap base;
    base["tracked.md"] = BaselineEntry{2, 2, 0x3};
    wc.set_baseline(base);

    wc.apply_events({make_event(FsEventKind::Modified, "tracked.md"), // ベースラインあり → ±
                     make_event(FsEventKind::Created, "fresh.md")});  // ベースラインなし → ◆

    const auto nf = wc.new_files();
    ASSERT_EQ(nf.size(), 1u);
    EXPECT_EQ(nf[0], "fresh.md");
}

// ---- ViewModel への合流：差分あり（±）と伝播（±淡）を区別する ----

TEST(WorkspaceControllerTest, ViewModelDistinguishesDiffAndPropagated)
{
    using pika::core::workspace::build_tree;
    using pika::core::workspace::Entry;

    WorkspaceController wc("C:/ws");
    BaselineMap base;
    base["dir/child.md"] = BaselineEntry{4, 4, 0x5};
    wc.set_baseline(base);
    wc.apply_events({make_event(FsEventKind::Modified, "dir/child.md")});

    std::vector<Entry> entries{{"dir", true}, {"dir/child.md", false}};
    const auto tree = build_tree(entries, {});
    const TreeRowVm vm = wc.build_view_model(tree);

    // ルート直下の dir フォルダ＝伝播（DiffPropagated・淡）。
    ASSERT_EQ(vm.children.size(), 1u);
    const TreeRowVm& dir = vm.children[0];
    EXPECT_TRUE(dir.is_dir);
    EXPECT_EQ(dir.mark, StateMark::DiffPropagated);

    // その配下の child.md＝差分あり（Diff・実心）。ベースラインありなので New ではない。
    ASSERT_EQ(dir.children.size(), 1u);
    EXPECT_EQ(dir.children[0].mark, StateMark::Diff);
}

TEST(WorkspaceControllerTest, ViewModelShowsNewMarkForBaselinelessUnread)
{
    using pika::core::workspace::build_tree;
    using pika::core::workspace::Entry;

    WorkspaceController wc("C:/ws");
    wc.apply_events({make_event(FsEventKind::Created, "brand-new.md")});

    std::vector<Entry> entries{{"brand-new.md", false}};
    const auto tree = build_tree(entries, {});
    const TreeRowVm vm = wc.build_view_model(tree);
    ASSERT_EQ(vm.children.size(), 1u);
    EXPECT_EQ(vm.children[0].mark, StateMark::New); // 新規＝◆
}

// ---- 自己保存トークン照合（WatcherCore::poll で消し込み）→ 未読化しないことを観測 ----
// 自己保存抑制の主条件は「ディスク内容ハッシュ一致」（ワンショット）。一致したら poll は FsEvent を
// 出さず WorkspaceController も未読化しない。不一致なら外部変更として未読化する（design 5.2）。

TEST(WorkspaceControllerTest, SelfSaveSuppressedDoesNotMarkUnread)
{
    constexpr std::uint64_t kSavedHash = 0xCAFE;
    // ディスク内容ハッシュは「保存後ハッシュ」と一致する（＝pika 自身の保存）。
    WatcherCore core([](const std::string&) -> std::optional<std::uint64_t> { return kSavedHash; });
    WorkspaceController wc("C:/ws");

    core.register_self_save("self.md", kSavedHash, /*at*/ 1000);
    RawEvent raw;
    raw.action = RawAction::Modified;
    raw.path = "self.md";
    raw.at = 1010;
    core.on_raw(raw);

    // デバウンス窓を越えた時刻で poll する（合成が確定する）。
    const std::vector<FsEvent> events = core.poll(/*now*/ 2000);
    const auto changes = wc.apply_events(events);

    // 自己保存はハッシュ一致で抑制され FsEvent が出ない → 未読化しない。
    EXPECT_TRUE(events.empty());
    EXPECT_TRUE(changes.empty());
    EXPECT_FALSE(wc.unread().is_unread("self.md"));
}

TEST(WorkspaceControllerTest, ExternalChangeMarksUnreadWhenHashDiffers)
{
    constexpr std::uint64_t kSavedHash = 0xCAFE;
    constexpr std::uint64_t kDiskHash = 0xBEEF; // 保存後ハッシュと異なる＝外部の上書き。
    WatcherCore core([](const std::string&) -> std::optional<std::uint64_t> { return kDiskHash; });
    WorkspaceController wc("C:/ws");

    core.register_self_save("ext.md", kSavedHash, /*at*/ 1000);
    RawEvent raw;
    raw.action = RawAction::Modified;
    raw.path = "ext.md";
    raw.at = 1010;
    core.on_raw(raw);

    const std::vector<FsEvent> events = core.poll(/*now*/ 2000);
    const auto changes = wc.apply_events(events);

    // ハッシュ不一致＝外部変更。FsEvent が出て未読化される。
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, FsEventKind::Modified);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_TRUE(wc.unread().is_unread("ext.md"));
}

} // namespace
