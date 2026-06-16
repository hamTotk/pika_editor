// controller/tab_manager の検証（sprint2 must）。
// - TabManager: open/close/activate と重畳状態の表示優先（削除済み ＞ 未保存 ＞
// 差分あり。要件5.3）。
// - 消失タブ（パス消滅）は削除済み表示へ安全遷移しクラッシュしない（design.md 5.1 手順4）。
// - フォルダ切替の状態機械（design.md 5.6。should）。
#include "controller/tab_manager.h"
#include "controller/tree_view_model.h"

#include <gtest/gtest.h>

namespace
{

using pika::controller::display_mark;
using pika::controller::FolderSwitch;
using pika::controller::FolderSwitchPhase;
using pika::controller::StateMark;
using pika::controller::TabManager;
using pika::controller::TabState;
using pika::controller::UnsavedChoice;

// ---- open / activate ----

TEST(TabManagerTest, OpenAddsTabAndActivates)
{
    TabManager tm;
    EXPECT_TRUE(tm.empty());
    std::size_t i = tm.open("C:\\a.md", "a.md");
    EXPECT_EQ(i, 0u);
    EXPECT_EQ(tm.count(), 1u);
    EXPECT_EQ(tm.active_index(), 0u);
    ASSERT_NE(tm.at(0), nullptr);
    EXPECT_EQ(tm.at(0)->path, "C:\\a.md");
}

TEST(TabManagerTest, OpenSamePathDoesNotDuplicate)
{
    TabManager tm;
    tm.open("C:\\a.md", "a.md");
    tm.open("C:\\b.md", "b.md");
    EXPECT_EQ(tm.count(), 2u);
    EXPECT_EQ(tm.active_index(), 1u);

    // 既存 a.md を再オープン → 重複せずアクティブだけ移る。
    std::size_t i = tm.open("C:\\a.md", "a.md");
    EXPECT_EQ(i, 0u);
    EXPECT_EQ(tm.count(), 2u);
    EXPECT_EQ(tm.active_index(), 0u);
}

TEST(TabManagerTest, ActivateOutOfRangeIsIgnored)
{
    TabManager tm;
    tm.open("C:\\a.md", "a.md");
    tm.activate(99);                  // 範囲外
    EXPECT_EQ(tm.active_index(), 0u); // 変わらない（クラッシュしない）
}

// ---- close と安全なアクティブ遷移 ----

TEST(TabManagerTest, CloseActiveMovesToRightNeighbor)
{
    TabManager tm;
    tm.open("C:\\a.md", "a.md"); // 0
    tm.open("C:\\b.md", "b.md"); // 1
    tm.open("C:\\c.md", "c.md"); // 2
    tm.activate(1);              // b をアクティブに
    tm.close(1);                 // b を閉じる → 右隣 c が同 index 1 に繰り上がる
    EXPECT_EQ(tm.count(), 2u);
    EXPECT_EQ(tm.active_index(), 1u);
    ASSERT_NE(tm.at(1), nullptr);
    EXPECT_EQ(tm.at(1)->path, "C:\\c.md");
}

TEST(TabManagerTest, CloseLastActiveFallsBackToLeft)
{
    TabManager tm;
    tm.open("C:\\a.md", "a.md"); // 0
    tm.open("C:\\b.md", "b.md"); // 1
    tm.activate(1);
    tm.close(1); // 末尾を閉じたら左隣へ
    EXPECT_EQ(tm.active_index(), 0u);
    EXPECT_EQ(tm.at(0)->path, "C:\\a.md");
}

TEST(TabManagerTest, CloseNonActiveLeftKeepsActiveTab)
{
    TabManager tm;
    tm.open("C:\\a.md", "a.md"); // 0
    tm.open("C:\\b.md", "b.md"); // 1
    tm.open("C:\\c.md", "c.md"); // 2 (active)
    tm.close(0);                 // アクティブより左を閉じる
    EXPECT_EQ(tm.count(), 2u);
    // 依然として c.md がアクティブ（index は 1 に繰り上がる）。
    ASSERT_NE(tm.at(tm.active_index()), nullptr);
    EXPECT_EQ(tm.at(tm.active_index())->path, "C:\\c.md");
}

TEST(TabManagerTest, CloseAllLeavesNoActive)
{
    TabManager tm;
    tm.open("C:\\a.md", "a.md");
    tm.close(0);
    EXPECT_TRUE(tm.empty());
    EXPECT_EQ(tm.active_index(), TabManager::kNoActive);
}

TEST(TabManagerTest, CloseOutOfRangeIsIgnored)
{
    TabManager tm;
    tm.open("C:\\a.md", "a.md");
    tm.close(50); // 範囲外 → 何もしない
    EXPECT_EQ(tm.count(), 1u);
    EXPECT_EQ(tm.active_index(), 0u);
}

// ---- 消失タブの安全遷移（design.md 5.1 手順4） ----

TEST(TabManagerTest, MissingPathBecomesDeletedAndStaysOpen)
{
    TabManager tm;
    tm.open("C:\\a.md", "a.md");
    tm.mark_path_missing("C:\\a.md");
    // 閉じない（未確認内容を保持）。表示マークは削除済み（取り消し線）。
    EXPECT_EQ(tm.count(), 1u);
    ASSERT_NE(tm.at(0), nullptr);
    EXPECT_TRUE(tm.at(0)->path_missing);
    EXPECT_EQ(display_mark(*tm.at(0)), StateMark::Deleted);
}

TEST(TabManagerTest, MissingActiveTabDoesNotCrashAndKeepsActive)
{
    TabManager tm;
    tm.open("C:\\a.md", "a.md");
    tm.open("C:\\b.md", "b.md"); // active
    tm.mark_path_missing("C:\\b.md");
    EXPECT_EQ(tm.active_index(), 1u); // アクティブのまま（削除済み表示で残す）
    EXPECT_EQ(display_mark(*tm.at(1)), StateMark::Deleted);
}

TEST(TabManagerTest, MarkMissingUnknownPathIsNoop)
{
    TabManager tm;
    tm.open("C:\\a.md", "a.md");
    tm.mark_path_missing("C:\\nope.md"); // 該当なし → 何もしない
    EXPECT_FALSE(tm.at(0)->path_missing);
}

// ---- 重畳状態の表示優先（削除済み ＞ 未保存 ＞ 差分あり。要件5.3・ui-design 5章） ----

TEST(DisplayMarkTest, DeletedBeatsUnsavedAndUnread)
{
    TabState t;
    t.path_missing = true;
    t.unsaved = true;
    t.unread = true;
    EXPECT_EQ(display_mark(t), StateMark::Deleted);
}

TEST(DisplayMarkTest, UnsavedBeatsUnread)
{
    TabState t;
    t.unsaved = true;
    t.unread = true;
    EXPECT_EQ(display_mark(t), StateMark::Unsaved);
}

TEST(DisplayMarkTest, UnreadWithBaselineIsDiff)
{
    TabState t;
    t.unread = true;
    t.has_baseline = true;
    EXPECT_EQ(display_mark(t), StateMark::Diff);
}

TEST(DisplayMarkTest, UnreadWithoutBaselineIsNew)
{
    TabState t;
    t.unread = true;
    t.has_baseline = false;
    EXPECT_EQ(display_mark(t), StateMark::New);
}

TEST(DisplayMarkTest, CleanTabHasNoMark)
{
    TabState t;
    EXPECT_EQ(display_mark(t), StateMark::None);
}

// set_unsaved / set_unread を通した重畳も同じ優先で解決される。
TEST(TabManagerTest, SettersDriveOverlayPriority)
{
    TabManager tm;
    tm.open("C:\\a.md", "a.md");
    tm.set_unread("C:\\a.md", true, true);
    EXPECT_EQ(display_mark(*tm.at(0)), StateMark::Diff);
    tm.set_unsaved("C:\\a.md", true);
    EXPECT_EQ(display_mark(*tm.at(0)), StateMark::Unsaved); // 未保存が差分に勝つ
    tm.mark_path_missing("C:\\a.md");
    EXPECT_EQ(display_mark(*tm.at(0)), StateMark::Deleted); // 削除済みが最優先
}

// ---- フォルダ切替の状態機械（design.md 5.6。should） ----

TEST(FolderSwitchTest, NoUnsavedGoesStraightToTeardown)
{
    FolderSwitch fs;
    EXPECT_EQ(fs.begin(false), FolderSwitchPhase::TeardownCurrent);
    EXPECT_EQ(fs.teardown_done(), FolderSwitchPhase::EnumerateNew);
}

TEST(FolderSwitchTest, UnsavedSaveAllProceeds)
{
    FolderSwitch fs;
    EXPECT_EQ(fs.begin(true), FolderSwitchPhase::ConfirmUnsaved);
    EXPECT_EQ(fs.resolve_unsaved(UnsavedChoice::SaveAll), FolderSwitchPhase::TeardownCurrent);
    EXPECT_EQ(fs.teardown_done(), FolderSwitchPhase::EnumerateNew);
}

TEST(FolderSwitchTest, UnsavedDiscardProceeds)
{
    FolderSwitch fs;
    fs.begin(true);
    EXPECT_EQ(fs.resolve_unsaved(UnsavedChoice::Discard), FolderSwitchPhase::TeardownCurrent);
}

TEST(FolderSwitchTest, CancelAbortsSwitch)
{
    FolderSwitch fs;
    fs.begin(true);
    EXPECT_EQ(fs.resolve_unsaved(UnsavedChoice::Cancel), FolderSwitchPhase::Cancelled);
    // 後始末は走らない（現ワークスペース維持）。
    EXPECT_EQ(fs.teardown_done(), FolderSwitchPhase::Cancelled);
}

TEST(FolderSwitchTest, ResolveUnsavedIgnoredOutsideConfirm)
{
    FolderSwitch fs;
    fs.begin(false); // TeardownCurrent
    // 確認段階でないので選択は無視される（安全側）。
    EXPECT_EQ(fs.resolve_unsaved(UnsavedChoice::Cancel), FolderSwitchPhase::TeardownCurrent);
}

} // namespace
