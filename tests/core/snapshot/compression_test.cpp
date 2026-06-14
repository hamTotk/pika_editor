// core/snapshot 圧縮の検証（sprint5 must「テキストを zstd 圧縮して objects に格納し、復元で原内容と
// 一致する」の圧縮層）。圧縮→復元がロスレス（バイト一致）で、破損入力を黙って空に倒さないことを観測する。
#include "core/snapshot/compression.h"

#include <string>

#include <gtest/gtest.h>

namespace
{

using pika::core::snapshot::compress;
using pika::core::snapshot::decompress;
using pika::util::ErrorCode;

TEST(CompressionTest, RoundTripsText)
{
    const std::string original =
        "# 見出し\n本文です。\r\nタブ\tと CRLF/LF 混在\n```\ncode block\n```\n";
    auto c = compress(original);
    ASSERT_TRUE(c.is_ok());
    auto d = decompress(c.value());
    ASSERT_TRUE(d.is_ok());
    EXPECT_EQ(d.value(), original);
}

TEST(CompressionTest, RoundTripsEmpty)
{
    auto c = compress("");
    ASSERT_TRUE(c.is_ok());
    auto d = decompress(c.value());
    ASSERT_TRUE(d.is_ok());
    EXPECT_TRUE(d.value().empty());
}

TEST(CompressionTest, RoundTripsBinaryWithNuls)
{
    std::string data;
    data.push_back('\0');
    data += "mid";
    data.push_back('\0');
    data += "end";
    auto c = compress(data);
    ASSERT_TRUE(c.is_ok());
    auto d = decompress(c.value());
    ASSERT_TRUE(d.is_ok());
    EXPECT_EQ(d.value(), data);
    EXPECT_EQ(d.value().size(), data.size());
}

TEST(CompressionTest, RoundTripsLargeRepetitive)
{
    std::string big;
    for (int i = 0; i < 100000; ++i)
    {
        big += "line ";
        big += std::to_string(i);
        big += "\n";
    }
    auto c = compress(big);
    ASSERT_TRUE(c.is_ok());
    EXPECT_LT(c.value().size(), big.size()); // 圧縮されている
    auto d = decompress(c.value());
    ASSERT_TRUE(d.is_ok());
    EXPECT_EQ(d.value(), big);
}

TEST(CompressionTest, RejectsCorruptFrameInsteadOfReturningEmpty)
{
    // 圧縮済みでないバイト列は zstd フレームとして不正 → Io エラー（黙って空を返さない）。
    auto d = decompress("not a zstd frame at all");
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.code(), ErrorCode::Io);
}

} // namespace
