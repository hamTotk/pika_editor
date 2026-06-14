// core/watcher パイプライン結合の検証。
// 合成→rename正規化→自己保存抑制が一体で正しく確定イベントを出すことを観測する（design.md 5.2）。
// 自己保存判定の「現ディスク内容ハッシュ」は注入関数（モックディスク）で与え決定論化する。
#include "core/watcher/watcher_core.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

namespace
{

using pika::core::watcher::FsEvent;
using pika::core::watcher::FsEventKind;
using pika::core::watcher::RawAction;
using pika::core::watcher::RawEvent;
using pika::core::watcher::WatcherCore;

// モックディスク: パス→現在の LF 正規化ハッシュ。未登録は nullopt（不在/読めない）。
struct MockDisk
{
    std::unordered_map<std::string, std::uint64_t> hashes;
    std::optional<std::uint64_t> probe(const std::string& path) const
    {
        auto it = hashes.find(path);
        if (it == hashes.end())
        {
            return std::nullopt;
        }
        return it->second;
    }
};

RawEvent raw(RawAction a, const char* path, pika::core::watcher::TimeMs at)
{
    return RawEvent{a, path, at};
}

TEST(WatcherCoreTest, CoalescedExternalWriteSurfacesAsModified)
{
    MockDisk disk;
    disk.hashes["a.md"] = 0x9999; // 外部が書いた内容（保存後ハッシュとは異なる）
    WatcherCore w([&disk](const std::string& p) { return disk.probe(p); }, 100, 200);

    w.on_raw(raw(RawAction::Modified, "a.md", 0));
    w.on_raw(raw(RawAction::Modified, "a.md", 20));
    auto out = w.poll(200);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].path, "a.md");
    EXPECT_EQ(out[0].kind, FsEventKind::Modified);
}

TEST(WatcherCoreTest, SelfSaveIsSuppressedWhenDiskHashMatches)
{
    MockDisk disk;
    disk.hashes["a.md"] = 0xABCD; // 保存後ハッシュと一致＝自己保存
    WatcherCore w([&disk](const std::string& p) { return disk.probe(p); }, 100, 200);

    // 保存直前にトークン登録 → その後 watcher が自分の保存イベントを観測。
    w.register_self_save("a.md", 0xABCD, 0);
    w.on_raw(raw(RawAction::Modified, "a.md", 5));
    auto out = w.poll(200);
    EXPECT_TRUE(out.empty()); // 自己保存は抑制される（未読化しない）
}

TEST(WatcherCoreTest, ExternalChangeAfterSaveIsNotSuppressed)
{
    MockDisk disk;
    WatcherCore w([&disk](const std::string& p) { return disk.probe(p); }, 100, 200);

    // 自己保存トークン(0xABCD)を登録したが、ディスクには外部が別内容(0x9999)を書いた。
    w.register_self_save("a.md", 0xABCD, 0);
    disk.hashes["a.md"] = 0x9999;
    w.on_raw(raw(RawAction::Modified, "a.md", 5));
    auto out = w.poll(200);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].kind, FsEventKind::Modified); // 外部変更として扱う
}

TEST(WatcherCoreTest, RenamePairSurfacesAsRenamed)
{
    MockDisk disk;
    WatcherCore w([&disk](const std::string& p) { return disk.probe(p); }, 100, 200);

    w.on_raw(raw(RawAction::RenamedOld, "old.md", 0));
    w.on_raw(raw(RawAction::RenamedNew, "new.md", 30));
    auto out = w.poll(50);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].kind, FsEventKind::Renamed);
    EXPECT_EQ(out[0].old_path, "old.md");
    EXPECT_EQ(out[0].path, "new.md");
}

TEST(WatcherCoreTest, LoneRenameOldBecomesRemovedNotSuppressed)
{
    MockDisk disk;
    // 削除は内容ハッシュで判定しない（自己保存トークンがあっても削除は握り潰さない）。
    WatcherCore w([&disk](const std::string& p) { return disk.probe(p); }, 100, 200);
    w.register_self_save("old.md", 0xABCD, 0);
    w.on_raw(raw(RawAction::RenamedOld, "old.md", 0));
    auto out = w.poll(300); // 相方 NEW が来ず窓超過→削除確定
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].kind, FsEventKind::Removed);
    EXPECT_EQ(out[0].path, "old.md");
}

TEST(WatcherCoreTest, DrainForResyncDropsPendingPartials)
{
    MockDisk disk;
    disk.hashes["a.md"] = 0x1;
    WatcherCore w([&disk](const std::string& p) { return disk.probe(p); }, 100, 200);
    w.on_raw(raw(RawAction::Modified, "a.md", 0));
    w.on_raw(raw(RawAction::RenamedOld, "b.md", 0));
    // 再同期に入る前に保留中の部分イベントを捨てる（再列挙が全状態を再構成するため）。
    w.drain_for_resync();
    auto out = w.poll(1000);
    EXPECT_TRUE(out.empty());
}

TEST(WatcherCoreTest, EventsReturnedInTimeOrder)
{
    MockDisk disk;
    disk.hashes["a.md"] = 0x1;
    disk.hashes["b.md"] = 0x2;
    WatcherCore w([&disk](const std::string& p) { return disk.probe(p); }, 100, 200);
    // rename ペア(確定時刻30) と 合成(確定時刻0,5) を混在させ、時刻昇順で返ることを確認する。
    w.on_raw(raw(RawAction::Modified, "a.md", 0));
    w.on_raw(raw(RawAction::Modified, "b.md", 5));
    w.on_raw(raw(RawAction::RenamedOld, "old.md", 30));
    w.on_raw(raw(RawAction::RenamedNew, "new.md", 30));
    auto out = w.poll(200);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_LE(out[0].at, out[1].at);
    EXPECT_LE(out[1].at, out[2].at);
}

} // namespace
