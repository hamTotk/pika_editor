#include "util/hash.h"

#include <array>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace pika::util
{

std::uint64_t xxh3_64(std::string_view data) noexcept
{
    return XXH3_64bits(data.data(), data.size());
}

namespace
{

std::string to_hex16(std::uint64_t h)
{
    static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string out(16, '0');
    for (int i = 0; i < 16; ++i)
    {
        // 上位ニブルから順に詰める（大端の16進表記）。
        const int shift = (15 - i) * 4;
        out[static_cast<std::size_t>(i)] = kHex[(h >> shift) & 0xF];
    }
    return out;
}

// CRLF を LF に畳んだコピーを返す（LF・CR 単独はそのまま）。
std::string normalize_lf(std::string_view data)
{
    std::string out;
    out.reserve(data.size());
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        if (data[i] == '\r' && i + 1 < data.size() && data[i + 1] == '\n')
        {
            continue; // 直後の '\n' を採用して CR を落とす
        }
        out.push_back(data[i]);
    }
    return out;
}

} // namespace

std::string xxh3_64_hex(std::string_view data)
{
    return to_hex16(xxh3_64(data));
}

std::uint64_t xxh3_64_lf(std::string_view data) noexcept
{
    return xxh3_64(normalize_lf(data));
}

std::string xxh3_64_lf_hex(std::string_view data)
{
    return to_hex16(xxh3_64_lf(data));
}

} // namespace pika::util
