// controller/baseline_merge の検証（F-013・段階1）。
// build_baseline_from_disk のディスク由来ベースラインへ index.json の確認済み永続ベースラインを
// rel 単位で上書きマージする純ロジックを観測する（wx・実 FS 非依存）。
#include "controller/baseline_merge.h"
#include "core/snapshot/snapshot_types.h"
#include "core/watcher/resync.h"
#include "util/hash.h"

#include <gtest/gtest.h>

namespace
{

using pika::controller::merge_index_into_baseline;
using pika::core::snapshot::IndexEntry;
using pika::core::snapshot::SnapshotIndex;
using pika::core::watcher::BaselineEntry;
using pika::core::watcher::BaselineMap;

// content_hash_lf が返す uint64 と xxh3_64_lf_hex の16進が同一の LF 正規化 XXH3-64 であることを
// 前提にマージは parse_hex で復元する。本テストはその往復が一致する想定を固定する。
TEST(BaselineMergeTest, HexRoundTripsToContentHashValue)
{
    const std::string content = "line1\nline2\n";
    const std::uint64_t v = pika::util::xxh3_64_lf(content);
    const std::string hex = pika::util::xxh3_64_lf_hex(content);
    EXPECT_EQ(std::stoull(hex, nullptr, 16), v);
}

TEST(BaselineMergeTest, OverwritesPresentRelWithIndexBaseline)
{
    BaselineMap disk;
    disk["a.md"] = BaselineEntry{10, 1234, /*hash_lf=*/0}; // ディスク由来（hash 無し）

    const std::string content = "confirmed content\n";
    SnapshotIndex index;
    IndexEntry e;
    e.rel_path = "a.md";
    e.baseline_hash = pika::util::xxh3_64_lf_hex(content);
    e.baseline_size = 99;
    e.baseline_mtime = 5555;
    index.entries.push_back(e);

    auto merged = merge_index_into_baseline(disk, index);

    ASSERT_EQ(merged.count("a.md"), 1u);
    EXPECT_EQ(merged["a.md"].size, 99u);
    EXPECT_EQ(merged["a.md"].mtime_ns, 5555u);
    EXPECT_EQ(merged["a.md"].hash_lf, pika::util::xxh3_64_lf(content)); // hex から復元した同一値
}

TEST(BaselineMergeTest, SkipsRelNotPresentOnDisk)
{
    BaselineMap disk; // 空＝ディスクに無い
    SnapshotIndex index;
    IndexEntry e;
    e.rel_path = "gone.md";
    e.baseline_hash = pika::util::xxh3_64_lf_hex("x");
    e.baseline_size = 1;
    e.baseline_mtime = 1;
    index.entries.push_back(e);

    auto merged = merge_index_into_baseline(disk, index);
    EXPECT_EQ(merged.count("gone.md"), 0u); // 足さない（resync が Removed を出す責務）
}

TEST(BaselineMergeTest, SkipsEntriesWithEmptyOrInvalidHash)
{
    BaselineMap disk;
    disk["empty.md"] = BaselineEntry{1, 2, 0};
    disk["bad.md"] = BaselineEntry{3, 4, 0};

    SnapshotIndex index;
    IndexEntry empty;
    empty.rel_path = "empty.md";
    empty.baseline_hash = ""; // ハッシュ無し
    empty.baseline_size = 100;
    index.entries.push_back(empty);
    IndexEntry bad;
    bad.rel_path = "bad.md";
    bad.baseline_hash = "zzzznothex"; // 不正16進
    bad.baseline_size = 200;
    index.entries.push_back(bad);

    auto merged = merge_index_into_baseline(disk, index);

    // どちらも上書きされずディスク由来のまま（取り繕わない）。
    EXPECT_EQ(merged["empty.md"].size, 1u);
    EXPECT_EQ(merged["empty.md"].hash_lf, 0u);
    EXPECT_EQ(merged["bad.md"].size, 3u);
    EXPECT_EQ(merged["bad.md"].hash_lf, 0u);
}

} // namespace
