#include "util/image_header.h"

#include <cstring>

namespace pika::util
{

namespace
{

std::uint8_t at(std::string_view b, std::size_t i) noexcept
{
    return static_cast<std::uint8_t>(b[i]);
}

// big-endian uint32（PNG の IHDR 寸法）。
std::uint32_t read_be32(std::string_view b, std::size_t off) noexcept
{
    return (static_cast<std::uint32_t>(at(b, off)) << 24) |
           (static_cast<std::uint32_t>(at(b, off + 1)) << 16) |
           (static_cast<std::uint32_t>(at(b, off + 2)) << 8) |
           static_cast<std::uint32_t>(at(b, off + 3));
}

// little-endian uint16（GIF の論理画面寸法）。
std::uint16_t read_le16(std::string_view b, std::size_t off) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(at(b, off)) |
                                      (static_cast<std::uint32_t>(at(b, off + 1)) << 8));
}

// little-endian int32（BMP の BITMAPINFOHEADER 寸法。height は負値=トップダウン）。
std::int32_t read_le32_signed(std::string_view b, std::size_t off) noexcept
{
    const std::uint32_t u = static_cast<std::uint32_t>(at(b, off)) |
                            (static_cast<std::uint32_t>(at(b, off + 1)) << 8) |
                            (static_cast<std::uint32_t>(at(b, off + 2)) << 16) |
                            (static_cast<std::uint32_t>(at(b, off + 3)) << 24);
    return static_cast<std::int32_t>(u);
}

std::uint32_t abs_i32(std::int32_t v) noexcept
{
    // -INT32_MIN の UB を避けるため unsigned で否定する。
    return v < 0 ? static_cast<std::uint32_t>(0u - static_cast<std::uint32_t>(v))
                 : static_cast<std::uint32_t>(v);
}

bool starts_with(std::string_view b, const unsigned char* sig, std::size_t n) noexcept
{
    if (b.size() < n)
    {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        if (at(b, i) != sig[i])
        {
            return false;
        }
    }
    return true;
}

} // namespace

ImageDimensions parse_image_dimensions(std::string_view bytes)
{
    ImageDimensions out;

    // ---- PNG: 8B シグネチャ → IHDR の width(16)/height(20)。big-endian uint32。----
    static const unsigned char kPng[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (starts_with(bytes, kPng, sizeof(kPng)) && bytes.size() >= 24)
    {
        const std::uint32_t w = read_be32(bytes, 16);
        const std::uint32_t h = read_be32(bytes, 20);
        if (w != 0 && h != 0)
        {
            out.known = true;
            out.width = w;
            out.height = h;
        }
        return out;
    }

    // ---- GIF: "GIF87a"/"GIF89a" → 論理画面 width(6-7)/height(8-9)。little-endian uint16。----
    static const unsigned char kGif87[6] = {'G', 'I', 'F', '8', '7', 'a'};
    static const unsigned char kGif89[6] = {'G', 'I', 'F', '8', '9', 'a'};
    if ((starts_with(bytes, kGif87, sizeof(kGif87)) ||
         starts_with(bytes, kGif89, sizeof(kGif89))) &&
        bytes.size() >= 10)
    {
        const std::uint16_t w = read_le16(bytes, 6);
        const std::uint16_t h = read_le16(bytes, 8);
        if (w != 0 && h != 0)
        {
            out.known = true;
            out.width = w;
            out.height = h;
        }
        return out;
    }

    // ---- BMP: "BM" → BITMAPINFOHEADER の width(18)/height(22)。little-endian int32。----
    // 旧 BITMAPCOREHEADER（12B・16bit 寸法）は扱わない（現代の出力はほぼ INFOHEADER。足さない）。
    static const unsigned char kBmp[2] = {'B', 'M'};
    if (starts_with(bytes, kBmp, sizeof(kBmp)) && bytes.size() >= 26)
    {
        const std::int32_t w = read_le32_signed(bytes, 18);
        const std::int32_t h = read_le32_signed(bytes, 22);
        const std::uint32_t aw = abs_i32(w);
        const std::uint32_t ah = abs_i32(h);
        if (aw != 0 && ah != 0)
        {
            out.known = true;
            out.width = aw;
            out.height = ah;
        }
        return out;
    }

    // それ以外（JPEG/WEBP/ICO/未知・短すぎ）は寸法不明＝呼び出し側フォールバックへ。
    return out;
}

} // namespace pika::util
