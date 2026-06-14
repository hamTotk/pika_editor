// core/watcher rename 正規化の検証（sprint3 must「rename 正規化」）。
// old/new ペアが時間窓内に揃えば追従、揃わない場合は安全側（old単独=削除、new単独=新規、
// 上書きrename=内容変更）に正規化することを観測する（design.md 5.2）。
#include "core/watcher/rename_tracker.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::watcher::FsEvent;
using pika::core::watcher::FsEventKind;
using pika::core::watcher::RenameTracker;

TEST(RenameTrackerTest, PairsOldNewWithinWindow)
{
    RenameTracker t(200);
    // OLD→NEW が窓内(50ms間隔)で揃う。
    EXPECT_TRUE(t.on_old("old.md", 0).empty());
    auto out = t.on_new("new.md", 50);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].kind, FsEventKind::Renamed);
    EXPECT_EQ(out[0].old_path, "old.md");
    EXPECT_EQ(out[0].path, "new.md");
    EXPECT_TRUE(t.empty());
}

TEST(RenameTrackerTest, PairsWhenNewArrivesFirst)
{
    RenameTracker t(200);
    // NEW が先に届くケース（OS の到着順は保証されない）。
    EXPECT_TRUE(t.on_new("new.md", 0).empty());
    auto out = t.on_old("old.md", 30);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].kind, FsEventKind::Renamed);
    EXPECT_EQ(out[0].old_path, "old.md");
    EXPECT_EQ(out[0].path, "new.md");
}

TEST(RenameTrackerTest, LoneOldBecomesRemoved)
{
    RenameTracker t(200);
    EXPECT_TRUE(t.on_old("gone.md", 0).empty());
    // 相方 NEW が来ないまま窓(200ms)超過→削除扱い。
    EXPECT_TRUE(t.flush_expired(100).empty()); // まだ窓内
    auto out = t.flush_expired(300);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].kind, FsEventKind::Removed);
    EXPECT_EQ(out[0].path, "gone.md");
}

TEST(RenameTrackerTest, LoneNewBecomesCreated)
{
    RenameTracker t(200);
    EXPECT_TRUE(t.on_new("appeared.md", 0).empty());
    auto out = t.flush_expired(300);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].kind, FsEventKind::Created);
    EXPECT_EQ(out[0].path, "appeared.md");
}

TEST(RenameTrackerTest, OutOfWindowOldNewDoNotPair)
{
    RenameTracker t(200);
    EXPECT_TRUE(t.on_old("old.md", 0).empty());
    // 窓(200ms)を超えた NEW はペアにしない（別操作扱い）→ NEW は保留され Created へ。
    auto immediate = t.on_new("new.md", 500);
    EXPECT_TRUE(immediate.empty());
    // 期限切れで old=Removed, new=Created が安全側に確定する。
    auto out = t.flush_expired(800);
    ASSERT_EQ(out.size(), 2u);
    // 時刻昇順（old=0→Removed, new=500→Created）。
    EXPECT_EQ(out[0].kind, FsEventKind::Removed);
    EXPECT_EQ(out[0].path, "old.md");
    EXPECT_EQ(out[1].kind, FsEventKind::Created);
    EXPECT_EQ(out[1].path, "new.md");
}

TEST(RenameTrackerTest, FlushAllDrainsPending)
{
    RenameTracker t(200);
    // 窓(200ms)を超えて離れた OLD/NEW は互いにペアにならず、各々が単独保留として残る。
    t.on_old("a.md", 0);
    t.on_new("b.md", 1000);
    // flush_all は窓を待たず保留を安全側（OLD=Removed, NEW=Created）に確定して捨てる。
    auto out = t.flush_all();
    ASSERT_EQ(out.size(), 2u);
    EXPECT_TRUE(t.empty());
}

} // namespace
