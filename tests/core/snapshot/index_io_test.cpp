// core/snapshot index.json の読み書きの検証（sprint5 must「index破損復元」/ design.md 7章 K2
// 「version・未知versionは安全側・アトミック書き込み」）。
// 往復・破損入力の分類・未知version安全側・アトミック書き込みを観測する。
#include "core/snapshot/index_io.h"

#include "util/atomic_file.h"

#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace
{

namespace fs = std::filesystem;

using pika::core::snapshot::IndexEntry;
using pika::core::snapshot::kIndexVersion;
using pika::core::snapshot::load_index;
using pika::core::snapshot::parse_index;
using pika::core::snapshot::save_index;
using pika::core::snapshot::serialize_index;
using pika::core::snapshot::SnapshotIndex;
using pika::core::snapshot::StashEntry;
using pika::core::snapshot::StashKind;
using pika::util::ErrorCode;

SnapshotIndex sample_index()
{
    SnapshotIndex idx;
    idx.version = kIndexVersion;
    IndexEntry e;
    e.rel_path = "docs/readme.md";
    e.baseline_hash = "00112233aabbccdd";
    e.baseline_object = "ffeeddccbbaa9988";
    e.baseline_mtime = 1700000000;
    e.baseline_size = 4096;
    e.unread = true;
    StashEntry s;
    s.hash = "0102030405060708";
    s.time = 1700000100;
    s.kind = StashKind::Incoming;
    s.batch_id = "";
    s.restored = false;
    e.stash.push_back(s);
    StashEntry s2;
    s2.hash = "1112131415161718";
    s2.time = 1700000200;
    s2.kind = StashKind::BaselineReplace;
    s2.batch_id = "batch-7";
    s2.restored = true;
    e.stash.push_back(s2);
    idx.entries.push_back(e);
    return idx;
}

TEST(IndexIoTest, SerializeParseRoundTrip)
{
    const SnapshotIndex original = sample_index();
    auto parsed = parse_index(serialize_index(original));
    ASSERT_TRUE(parsed.is_ok());
    const SnapshotIndex& got = parsed.value();
    EXPECT_EQ(got.version, original.version);
    ASSERT_EQ(got.entries.size(), 1u);
    const auto& e = got.entries[0];
    EXPECT_EQ(e.rel_path, "docs/readme.md");
    EXPECT_EQ(e.baseline_hash, "00112233aabbccdd");
    EXPECT_EQ(e.baseline_object, "ffeeddccbbaa9988");
    EXPECT_EQ(e.baseline_mtime, 1700000000);
    EXPECT_EQ(e.baseline_size, 4096u);
    EXPECT_TRUE(e.unread);
    ASSERT_EQ(e.stash.size(), 2u);
    EXPECT_EQ(e.stash[0].hash, "0102030405060708");
    EXPECT_EQ(e.stash[0].kind, StashKind::Incoming);
    EXPECT_FALSE(e.stash[0].restored);
    EXPECT_EQ(e.stash[1].kind, StashKind::BaselineReplace);
    EXPECT_EQ(e.stash[1].batch_id, "batch-7");
    EXPECT_TRUE(e.stash[1].restored);
}

TEST(IndexIoTest, BrokenJsonIsClassifiedAsIo)
{
    auto r = parse_index("{ this is not valid json ");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.code(), ErrorCode::Io);
}

TEST(IndexIoTest, MissingVersionIsRejected)
{
    auto r = parse_index(R"({"entries":[]})");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.code(), ErrorCode::Io);
}

TEST(IndexIoTest, UnknownNewerVersionFailsSafe)
{
    // 未知（新しい）version は読み込まず・再生成もせず安全側（K2）。
    const std::string future =
        R"({"version":)" + std::to_string(kIndexVersion + 1) + R"(,"entries":[]})";
    auto r = parse_index(future);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.code(), ErrorCode::Unsupported);
}

class IndexIoFsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        dir_ = fs::temp_directory_path() /
               ("pika_indexio_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(dir_);
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    std::string index_path() const { return (dir_ / "index.json").string(); }
    fs::path dir_;
};

TEST_F(IndexIoFsTest, SaveLoadRoundTrip)
{
    const SnapshotIndex original = sample_index();
    ASSERT_TRUE(save_index(index_path(), original).is_ok());

    auto loaded = load_index(index_path());
    ASSERT_TRUE(loaded.is_ok());
    ASSERT_EQ(loaded.value().entries.size(), 1u);
    EXPECT_EQ(loaded.value().entries[0].rel_path, "docs/readme.md");
}

TEST_F(IndexIoFsTest, LoadMissingReturnsFreshIndex)
{
    auto loaded = load_index(index_path()); // ファイルが存在しない＝初回オープン
    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(loaded.value().version, kIndexVersion);
    EXPECT_TRUE(loaded.value().entries.empty());
}

TEST_F(IndexIoFsTest, SaveIsAtomicNoTempLeftBehind)
{
    ASSERT_TRUE(save_index(index_path(), sample_index()).is_ok());
    int tmp = 0;
    for (const auto& e : fs::directory_iterator(dir_))
    {
        if (e.path().extension() == ".tmp")
        {
            ++tmp;
        }
    }
    EXPECT_EQ(tmp, 0); // 一時ファイルは rename で消費される（アトミック書き込み）
}

TEST_F(IndexIoFsTest, CorruptOnDiskIsClassifiedAsIo)
{
    // ディスク上の index.json が破損していても、load はエラーを返す（呼び出し側が復元へ進む）。
    ASSERT_TRUE(pika::util::write_atomic(index_path(), "{ broken ").is_ok());
    auto loaded = load_index(index_path());
    ASSERT_TRUE(loaded.is_err());
    EXPECT_EQ(loaded.code(), ErrorCode::Io);
}

} // namespace
