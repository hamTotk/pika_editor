// core/watcher fs_probe の検証（確定読み・再同期が依存する軽量メタ取得とハッシュ）。
// テンポラリフォルダの実 FS で、存在/非存在・サイズ・LF 正規化ハッシュの整合を観測する。
#include "core/watcher/fs_probe.h"

#include "util/atomic_file.h"
#include "util/hash.h"

#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace
{

namespace fs = std::filesystem;

using pika::core::watcher::content_hash_lf;
using pika::core::watcher::probe;

class FsProbeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        dir_ = fs::temp_directory_path() /
               ("pika_fsprobe_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(dir_);
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    std::string path_of(const char* name) const { return (dir_ / name).generic_string(); }
    fs::path dir_;
};

TEST_F(FsProbeTest, ProbeReportsExistenceAndSize)
{
    const std::string p = path_of("a.txt");
    ASSERT_TRUE(pika::util::write_atomic(p, "12345").is_ok());
    auto st = probe(p);
    EXPECT_TRUE(st.exists);
    EXPECT_EQ(st.size, 5u);
}

TEST_F(FsProbeTest, ProbeMissingFileIsAbsent)
{
    auto st = probe(path_of("missing.txt"));
    EXPECT_FALSE(st.exists);
    EXPECT_EQ(st.size, 0u);
}

TEST_F(FsProbeTest, ProbeDirectoryIsNotAContentFile)
{
    auto st = probe(dir_.generic_string());
    EXPECT_FALSE(st.exists); // ディレクトリは内容ファイルとして扱わない
}

TEST_F(FsProbeTest, ContentHashIsLfNormalized)
{
    const std::string p_lf = path_of("lf.txt");
    const std::string p_crlf = path_of("crlf.txt");
    ASSERT_TRUE(pika::util::write_atomic(p_lf, "a\nb\nc").is_ok());
    ASSERT_TRUE(pika::util::write_atomic(p_crlf, "a\r\nb\r\nc").is_ok());

    auto h_lf = content_hash_lf(p_lf);
    auto h_crlf = content_hash_lf(p_crlf);
    ASSERT_TRUE(h_lf.is_ok());
    ASSERT_TRUE(h_crlf.is_ok());
    // 改行コードのみ異なる同一内容は同じ LF 正規化ハッシュ。
    EXPECT_EQ(h_lf.value(), h_crlf.value());
    // util の LF 正規化ハッシュと一致する（同じ正規化規則）。
    EXPECT_EQ(h_lf.value(), pika::util::xxh3_64_lf("a\nb\nc"));
}

TEST_F(FsProbeTest, ContentHashMissingReturnsError)
{
    auto h = content_hash_lf(path_of("nope.txt"));
    EXPECT_TRUE(h.is_err());
}

} // namespace
