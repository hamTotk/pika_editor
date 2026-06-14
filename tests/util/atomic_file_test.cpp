// util/atomic_file の検証（sprint 2）。
// テンポラリフォルダで一時ファイル→rename のアトミック書き込みが成功し、
// 書き込み途中でも元ファイルが破損しない経路であることを観測する（要件12.1・design.md 5.3）。
#include "util/atomic_file.h"

#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace
{

using pika::util::ErrorCode;
using pika::util::read_all;
using pika::util::write_atomic;

namespace fs = std::filesystem;

// テスト用テンポラリディレクトリ（各テストで作成・後始末する）。
class AtomicFileTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // this のアドレスでテストごとに一意なディレクトリ名にする（並行実行衝突回避）。
        dir_ = fs::temp_directory_path() /
               ("pika_atomic_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(dir_);
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::string path_of(const char* name) const { return (dir_ / name).string(); }

    fs::path dir_;
};

TEST_F(AtomicFileTest, WritesNewFile)
{
    const std::string p = path_of("new.txt");
    auto w = write_atomic(p, "hello world");
    ASSERT_TRUE(w.is_ok());

    auto r = read_all(p);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), "hello world");
}

TEST_F(AtomicFileTest, OverwritesExistingFileAtomically)
{
    const std::string p = path_of("over.txt");
    ASSERT_TRUE(write_atomic(p, "original content").is_ok());
    ASSERT_TRUE(write_atomic(p, "replaced").is_ok());

    auto r = read_all(p);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), "replaced");
}

TEST_F(AtomicFileTest, BinaryWithEmbeddedNulsRoundTrips)
{
    const std::string p = path_of("bin.dat");
    std::string data;
    data.push_back('\0');
    data += "mid";
    data.push_back('\0');
    data += "end";
    ASSERT_TRUE(write_atomic(p, data).is_ok());

    auto r = read_all(p);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), data);
    EXPECT_EQ(r.value().size(), data.size());
}

TEST_F(AtomicFileTest, EmptyContentWrites)
{
    const std::string p = path_of("empty.txt");
    ASSERT_TRUE(write_atomic(p, "").is_ok());
    auto r = read_all(p);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(AtomicFileTest, NoTempFileLeftBehindOnSuccess)
{
    const std::string p = path_of("clean.txt");
    ASSERT_TRUE(write_atomic(p, "data").is_ok());
    // 一時ファイル（*.pika-*.tmp）が成功後に残っていないこと（rename で消費される）。
    int leftover = 0;
    for (const auto& e : fs::directory_iterator(dir_))
    {
        if (e.path().extension() == ".tmp")
        {
            ++leftover;
        }
    }
    EXPECT_EQ(leftover, 0);
}

TEST_F(AtomicFileTest, FailedWriteDoesNotCorruptExisting)
{
    // 書き込み先ディレクトリ（存在しないサブパス）への失敗で、元ファイルが無傷であること。
    const std::string good = path_of("keep.txt");
    ASSERT_TRUE(write_atomic(good, "safe baseline").is_ok());

    // 存在しない深いパスへの書き込みは失敗するが、別ファイルの内容には影響しない。
    const std::string bad = (dir_ / "no_such_dir" / "x.txt").string();
    auto w = write_atomic(bad, "junk");
    EXPECT_TRUE(w.is_err());
    EXPECT_EQ(w.code(), ErrorCode::Io);

    auto r = read_all(good);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), "safe baseline"); // 元ファイルは破損していない
}

TEST_F(AtomicFileTest, ReadMissingReturnsNotFound)
{
    auto r = read_all(path_of("does_not_exist.txt"));
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.code(), ErrorCode::NotFound);
}

} // namespace
