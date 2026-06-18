// controller/watch_event_map の検証（sprint4 must）。
// ReadDirectoryChangesW の FILE_NOTIFY_INFORMATION（Action コード＋'\\'区切り相対パス）を
// core/watcher RawEvent（RawAction＋'/'区切り UTF-8 相対パス）へ写す純ロジックを観測する。
#include "controller/watch_event_map.h"
#include "core/watcher/fs_event.h"

#include <gtest/gtest.h>

namespace
{

using pika::controller::kActionAdded;
using pika::controller::kActionModified;
using pika::controller::kActionRemoved;
using pika::controller::kActionRenamedNew;
using pika::controller::kActionRenamedOld;
using pika::controller::make_raw_event;
using pika::controller::map_watch_action;
using pika::controller::normalize_watch_rel_path;
using pika::core::watcher::RawAction;

// ---- Action コードの写像 ----

TEST(WatchEventMapTest, MapsKnownActions)
{
    EXPECT_EQ(map_watch_action(kActionAdded), RawAction::Added);
    EXPECT_EQ(map_watch_action(kActionRemoved), RawAction::Removed);
    EXPECT_EQ(map_watch_action(kActionModified), RawAction::Modified);
    EXPECT_EQ(map_watch_action(kActionRenamedOld), RawAction::RenamedOld);
    EXPECT_EQ(map_watch_action(kActionRenamedNew), RawAction::RenamedNew);
}

TEST(WatchEventMapTest, UnknownActionIsDropped)
{
    EXPECT_FALSE(map_watch_action(0).has_value());
    EXPECT_FALSE(map_watch_action(99).has_value());
}

// ---- パス正規化（'\\' → '/'・先頭/末尾区切り除去） ----

TEST(WatchEventMapTest, NormalizesBackslashToForwardSlash)
{
    EXPECT_EQ(normalize_watch_rel_path("dir\\sub\\file.md"), "dir/sub/file.md");
}

TEST(WatchEventMapTest, TrimsLeadingAndTrailingSeparators)
{
    EXPECT_EQ(normalize_watch_rel_path("\\dir\\file.md\\"), "dir/file.md");
    EXPECT_EQ(normalize_watch_rel_path("/leading"), "leading");
}

TEST(WatchEventMapTest, EmptyAndRootProduceEmpty)
{
    EXPECT_TRUE(normalize_watch_rel_path("").empty());
    EXPECT_TRUE(normalize_watch_rel_path("\\").empty());
    EXPECT_TRUE(normalize_watch_rel_path("/").empty());
}

TEST(WatchEventMapTest, PreservesUtf8MultibyteBytes)
{
    // 日本語ファイル名の UTF-8 バイト列を壊さない（区切りだけ置換する）。
    const std::string in = "フォルダ\\メモ.md";
    const std::string out = normalize_watch_rel_path(in);
    EXPECT_EQ(out, "フォルダ/メモ.md");
}

// ---- make_raw_event の合成 ----

TEST(WatchEventMapTest, MakeRawEventBuildsNormalizedEvent)
{
    const auto ev = make_raw_event(kActionModified, "dir\\a.md", /*at*/ 1234);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->action, RawAction::Modified);
    EXPECT_EQ(ev->path, "dir/a.md");
    EXPECT_EQ(ev->at, 1234u);
}

TEST(WatchEventMapTest, MakeRawEventDropsUnknownAction)
{
    EXPECT_FALSE(make_raw_event(0, "a.md", 1).has_value());
}

TEST(WatchEventMapTest, MakeRawEventDropsEmptyPath)
{
    // ルート自身・空パスは監視対象ファイルではない（WatcherCore へ投入しない）。
    EXPECT_FALSE(make_raw_event(kActionModified, "\\", 1).has_value());
    EXPECT_FALSE(make_raw_event(kActionAdded, "", 1).has_value());
}

TEST(WatchEventMapTest, MakeRawEventDropsPikaTempFile)
{
    // pika 自身のアトミック書き込み一時ファイルは未読/差分へ流さない（F-014「幽霊未読」対策）。
    EXPECT_FALSE(make_raw_event(kActionAdded, "report.md.pika-1234-5678.tmp", 1).has_value());
    EXPECT_FALSE(make_raw_event(kActionRemoved, "sub\\doc.md.pika-1-2.tmp", 1).has_value());
    // ユーザーの普通の `.tmp` は drop しない（イベントを返す）。
    EXPECT_TRUE(make_raw_event(kActionAdded, "notes.tmp", 1).has_value());
}

} // namespace
