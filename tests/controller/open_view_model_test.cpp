// controller/open_view_model の検証（F-022・要件12.2 I1/I2/I9・要件2.2）。
// open_file の種別分岐（画像/巨大画像/バイナリ/テキスト）を素値から決定論で解く。
#include "controller/open_view_model.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace
{

using pika::controller::OpenView;
using pika::controller::OpenViewInput;
using pika::controller::OpenViewResult;
using pika::controller::resolve_open_view;

// PNG（IHDR まで）を最小構成で作る（offset 16/20 に width/height BE）。
std::string make_png(std::uint32_t w, std::uint32_t h)
{
    std::string b;
    const unsigned char sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    b.append(reinterpret_cast<const char*>(sig), 8);
    const unsigned char len_ihdr[8] = {0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R'};
    b.append(reinterpret_cast<const char*>(len_ihdr), 8);
    b.push_back(static_cast<char>((w >> 24) & 0xFF));
    b.push_back(static_cast<char>((w >> 16) & 0xFF));
    b.push_back(static_cast<char>((w >> 8) & 0xFF));
    b.push_back(static_cast<char>(w & 0xFF));
    b.push_back(static_cast<char>((h >> 24) & 0xFF));
    b.push_back(static_cast<char>((h >> 16) & 0xFF));
    b.push_back(static_cast<char>((h >> 8) & 0xFF));
    b.push_back(static_cast<char>(h & 0xFF));
    return b;
}

// ---- テキスト ----

TEST(OpenViewModelTest, MarkdownIsText)
{
    OpenViewInput in;
    in.name_or_path = "C:/ws/readme.md";
    in.head = "# Title\n\nbody text\n";
    in.file_size = in.head.size();
    EXPECT_EQ(resolve_open_view(in).view, OpenView::Text);
}

TEST(OpenViewModelTest, PlainTextNoExtensionIsText)
{
    OpenViewInput in;
    in.name_or_path = "C:/ws/notes";
    in.head = "plain text without extension";
    in.file_size = in.head.size();
    EXPECT_EQ(resolve_open_view(in).view, OpenView::Text);
}

// ---- 画像（I1） ----

TEST(OpenViewModelTest, SmallPngIsImage)
{
    OpenViewInput in;
    in.name_or_path = "C:/ws/pic.png";
    in.head = make_png(1920, 1080);
    in.file_size = 200000;
    const OpenViewResult r = resolve_open_view(in);
    EXPECT_EQ(r.view, OpenView::Image);
    EXPECT_EQ(r.pixel_count, 1920ull * 1080ull);
}

TEST(OpenViewModelTest, ImageExtensionIsCaseInsensitive)
{
    OpenViewInput in;
    in.name_or_path = "C:/ws/PIC.PNG";
    in.head = make_png(100, 100);
    in.file_size = 5000;
    EXPECT_EQ(resolve_open_view(in).view, OpenView::Image);
}

// ---- 巨大画像（I2・要件2.2 総ピクセル数ガード） ----

TEST(OpenViewModelTest, HugePngIsImageTooLarge)
{
    OpenViewInput in;
    in.name_or_path = "C:/ws/huge.png";
    in.head = make_png(10000, 10000); // 1 億 px > 既定 6000万
    in.max_pixels = 60'000'000ull;
    in.file_size = 1000;
    EXPECT_EQ(resolve_open_view(in).view, OpenView::ImageTooLarge);
}

TEST(OpenViewModelTest, AtGuardBoundaryIsImage)
{
    // pixel_count == max_pixels はガード超過でない（resolve_degrade は > で判定）。
    OpenViewInput in;
    in.name_or_path = "C:/ws/edge.png";
    in.head = make_png(6000, 10000); // ちょうど 6000万 px
    in.max_pixels = 60'000'000ull;
    EXPECT_EQ(resolve_open_view(in).view, OpenView::Image);
}

TEST(OpenViewModelTest, SettingsRaisesPixelGuard)
{
    // max_pixels（settings.render_max_image_px）を上げると同じ画像が Image になる。
    OpenViewInput in;
    in.name_or_path = "C:/ws/big.png";
    in.head = make_png(10000, 10000); // 1 億 px
    in.max_pixels = 200'000'000ull;   // 緩和
    EXPECT_EQ(resolve_open_view(in).view, OpenView::Image);
}

// ---- 寸法不明画像のフォールバック（JPEG/WEBP/ICO） ----

TEST(OpenViewModelTest, JpegSmallFileFallsBackToImage)
{
    OpenViewInput in;
    in.name_or_path = "C:/ws/photo.jpg";
    in.head = "\xFF\xD8\xFF\xE0JFIF";  // JPEG・ヘッダ寸法は読まない
    in.file_size = 2u * 1024u * 1024u; // 2MB < 64MB
    EXPECT_EQ(resolve_open_view(in).view, OpenView::Image);
}

TEST(OpenViewModelTest, JpegHugeFileIsGuarded)
{
    OpenViewInput in;
    in.name_or_path = "C:/ws/huge.jpg";
    in.head = "\xFF\xD8\xFF\xE0JFIF";
    in.file_size = 128u * 1024u * 1024u; // 128MB > 64MB フォールバック上限
    EXPECT_EQ(resolve_open_view(in).view, OpenView::ImageTooLarge);
}

// ---- svg はラスタ簡易ビューの対象外（テキスト扱い＝XML として開く） ----

TEST(OpenViewModelTest, SvgIsTextNotImage)
{
    OpenViewInput in;
    in.name_or_path = "C:/ws/icon.svg";
    in.head = "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>";
    in.file_size = in.head.size();
    EXPECT_EQ(resolve_open_view(in).view, OpenView::Text);
}

// ---- バイナリ（I9） ----

TEST(OpenViewModelTest, BinaryWithNulIsUnsupported)
{
    OpenViewInput in;
    in.name_or_path = "C:/ws/data.bin";
    std::string b = "MZ";
    b.push_back('\0');
    b.append(200, '\x01');
    in.head = b;
    in.file_size = b.size();
    EXPECT_EQ(resolve_open_view(in).view, OpenView::Unsupported);
}

TEST(OpenViewModelTest, UnknownExtensionTextContentIsText)
{
    // 未知拡張子でも内容がテキストなら EditorPanel で開く（誤ってバイナリ扱いしない）。
    OpenViewInput in;
    in.name_or_path = "C:/ws/config.xyz";
    in.head = "key=value\nother=123\n";
    in.file_size = in.head.size();
    EXPECT_EQ(resolve_open_view(in).view, OpenView::Text);
}

} // namespace
