// util/image_header の検証（F-022・要件2.2/12.2）。
// PNG/GIF/BMP のヘッダ寸法を固定オフセットから読む（デコードしない）。それ以外/短すぎ/不正
// シグネチャは known=false（呼び出し側フォールバックへ）。
#include "util/image_header.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace
{

using pika::util::ImageDimensions;
using pika::util::parse_image_dimensions;

// PNG: 8B シグネチャ + IHDR チャンク（length(4)+"IHDR"(4)+width(4 BE)+height(4 BE)+...）。
// width/height は offset 16/20。本テストはそこまでのバイトを最小構成で組む。
std::string make_png(std::uint32_t w, std::uint32_t h)
{
    std::string b;
    const unsigned char sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    b.append(reinterpret_cast<const char*>(sig), 8);
    // IHDR length(13・BE) + "IHDR"
    const unsigned char len_ihdr[8] = {0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R'};
    b.append(reinterpret_cast<const char*>(len_ihdr), 8);
    // width(BE)
    b.push_back(static_cast<char>((w >> 24) & 0xFF));
    b.push_back(static_cast<char>((w >> 16) & 0xFF));
    b.push_back(static_cast<char>((w >> 8) & 0xFF));
    b.push_back(static_cast<char>(w & 0xFF));
    // height(BE)
    b.push_back(static_cast<char>((h >> 24) & 0xFF));
    b.push_back(static_cast<char>((h >> 16) & 0xFF));
    b.push_back(static_cast<char>((h >> 8) & 0xFF));
    b.push_back(static_cast<char>(h & 0xFF));
    return b;
}

// GIF: "GIF89a" + width(LE16) + height(LE16)。
std::string make_gif(std::uint16_t w, std::uint16_t h)
{
    std::string b = "GIF89a";
    b.push_back(static_cast<char>(w & 0xFF));
    b.push_back(static_cast<char>((w >> 8) & 0xFF));
    b.push_back(static_cast<char>(h & 0xFF));
    b.push_back(static_cast<char>((h >> 8) & 0xFF));
    return b;
}

// BMP: "BM" + ... + width(LE32 at 18) + height(LE32 at 22)。offset 2-17 はダミー。
std::string make_bmp(std::int32_t w, std::int32_t h)
{
    std::string b = "BM";
    b.append(16, '\0'); // offset 2..17（ファイルサイズ・予約・データオフセット・DIB ヘッダサイズ）
    const auto put_le32 = [&](std::int32_t v) {
        const std::uint32_t u = static_cast<std::uint32_t>(v);
        b.push_back(static_cast<char>(u & 0xFF));
        b.push_back(static_cast<char>((u >> 8) & 0xFF));
        b.push_back(static_cast<char>((u >> 16) & 0xFF));
        b.push_back(static_cast<char>((u >> 24) & 0xFF));
    };
    put_le32(w); // offset 18
    put_le32(h); // offset 22
    return b;
}

TEST(ImageHeaderTest, PngDimensions)
{
    const ImageDimensions d = parse_image_dimensions(make_png(1920, 1080));
    EXPECT_TRUE(d.known);
    EXPECT_EQ(d.width, 1920u);
    EXPECT_EQ(d.height, 1080u);
    EXPECT_EQ(d.pixel_count(), 1920ull * 1080ull);
}

TEST(ImageHeaderTest, PngLargeDoesNotOverflow)
{
    // 4 万 × 4 万 = 16 億 px > 32bit。pixel_count() は 64bit で正しく算出する。
    const ImageDimensions d = parse_image_dimensions(make_png(40000, 40000));
    EXPECT_TRUE(d.known);
    EXPECT_EQ(d.pixel_count(), 1'600'000'000ull);
}

TEST(ImageHeaderTest, GifDimensions)
{
    const ImageDimensions d = parse_image_dimensions(make_gif(640, 480));
    EXPECT_TRUE(d.known);
    EXPECT_EQ(d.width, 640u);
    EXPECT_EQ(d.height, 480u);
}

TEST(ImageHeaderTest, Gif87aAlsoParses)
{
    std::string b = "GIF87a";
    b.push_back(static_cast<char>(100));
    b.push_back(0);
    b.push_back(static_cast<char>(50));
    b.push_back(0);
    const ImageDimensions d = parse_image_dimensions(b);
    EXPECT_TRUE(d.known);
    EXPECT_EQ(d.width, 100u);
    EXPECT_EQ(d.height, 50u);
}

TEST(ImageHeaderTest, BmpDimensions)
{
    const ImageDimensions d = parse_image_dimensions(make_bmp(800, 600));
    EXPECT_TRUE(d.known);
    EXPECT_EQ(d.width, 800u);
    EXPECT_EQ(d.height, 600u);
}

TEST(ImageHeaderTest, BmpNegativeHeightIsAbsolute)
{
    // トップダウン DIB は height が負値。絶対値で寸法を返す（pixel_count を正しく出す）。
    const ImageDimensions d = parse_image_dimensions(make_bmp(800, -600));
    EXPECT_TRUE(d.known);
    EXPECT_EQ(d.width, 800u);
    EXPECT_EQ(d.height, 600u);
}

TEST(ImageHeaderTest, UnknownFormatJpegReturnsUnknown)
{
    // JPEG（SOI=FFD8）はヘッダ解析しない＝known=false でフォールバックへ。
    std::string jpeg = "\xFF\xD8\xFF\xE0";
    jpeg.append(64, '\0');
    const ImageDimensions d = parse_image_dimensions(jpeg);
    EXPECT_FALSE(d.known);
    EXPECT_EQ(d.pixel_count(), 0u);
}

TEST(ImageHeaderTest, TruncatedPngIsUnknown)
{
    // シグネチャだけで IHDR が欠けると寸法を取れない（known=false）。
    const unsigned char sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const std::string b(reinterpret_cast<const char*>(sig), 8);
    const ImageDimensions d = parse_image_dimensions(b);
    EXPECT_FALSE(d.known);
}

TEST(ImageHeaderTest, EmptyIsUnknown)
{
    const ImageDimensions d = parse_image_dimensions(std::string_view{});
    EXPECT_FALSE(d.known);
}

TEST(ImageHeaderTest, ZeroDimensionIsUnknown)
{
    // width=0 等の壊れたヘッダは known=false（0 を有効寸法として扱わない）。
    const ImageDimensions d = parse_image_dimensions(make_png(0, 100));
    EXPECT_FALSE(d.known);
}

} // namespace
