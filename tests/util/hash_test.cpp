// util/hash の XXH3 ハッシュのスモークテスト（sprint 1）。
// ビルド基盤が成立し ctest で 1 件以上 PASS することを担保する最小検証。
#include "util/hash.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace
{

TEST(Xxh3Hash, SameInputSameHash)
{
    EXPECT_EQ(pika::util::xxh3_64("pika"), pika::util::xxh3_64("pika"));
}

TEST(Xxh3Hash, DifferentInputDifferentHash)
{
    EXPECT_NE(pika::util::xxh3_64("pika"), pika::util::xxh3_64("pica"));
}

TEST(Xxh3Hash, EmptyInputIsStable)
{
    // 空入力でもクラッシュせず決定的な値を返す。
    EXPECT_EQ(pika::util::xxh3_64(""), pika::util::xxh3_64(std::string_view{}));
}

TEST(Xxh3Hash, HexIs16LowerHexChars)
{
    const std::string hex = pika::util::xxh3_64_hex("pika");
    ASSERT_EQ(hex.size(), 16u);
    for (char c : hex)
    {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << "non-hex char: " << c;
    }
}

TEST(Xxh3Hash, HexMatchesSnprintf)
{
    // hex 表記が標準の %016llx と一致することで桁詰め・バイトオーダーの取り違えを検出する。
    const std::uint64_t h = pika::util::xxh3_64("pika");
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    EXPECT_EQ(pika::util::xxh3_64_hex("pika"), std::string(buf));
}

// --- LF 正規化ハッシュ（未読判定・差分照合の土台。改行のみの差を出さない） ---

TEST(Xxh3LfHash, CrlfAndLfOnlyDifferenceMatches)
{
    // CRLF と LF のみ異なる同一内容は LF 正規化後ハッシュが一致する。
    const std::string crlf = "line1\r\nline2\r\nline3";
    const std::string lf = "line1\nline2\nline3";
    EXPECT_EQ(pika::util::xxh3_64_lf(crlf), pika::util::xxh3_64_lf(lf));
    EXPECT_EQ(pika::util::xxh3_64_lf_hex(crlf), pika::util::xxh3_64_lf_hex(lf));
}

TEST(Xxh3LfHash, ContentDifferenceStillDiffers)
{
    // 内容が違えば（改行以外で）ハッシュは異なる。
    EXPECT_NE(pika::util::xxh3_64_lf("line1\r\nline2"), pika::util::xxh3_64_lf("line1\r\nLINE2"));
}

TEST(Xxh3LfHash, MixedNewlinesNormalizeToLf)
{
    // 混在改行も LF 正規化後は LF 統一内容と一致する。
    const std::string mixed = "a\r\nb\nc\r\nd";
    const std::string lf = "a\nb\nc\nd";
    EXPECT_EQ(pika::util::xxh3_64_lf(mixed), pika::util::xxh3_64_lf(lf));
}

TEST(Xxh3LfHash, LoneCrIsNotNewlineNormalized)
{
    // CR 単独（\r のみ）は改行として正規化しない（CRLF のみ畳む）。LF 内容とは別物。
    EXPECT_NE(pika::util::xxh3_64_lf("a\rb"), pika::util::xxh3_64_lf("a\nb"));
}

} // namespace
