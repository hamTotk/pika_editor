#include "util/binary_detect.h"

#include <cstdint>

namespace pika::util
{

namespace
{

// 先頭が既知のテキスト BOM（UTF-8 / UTF-16 LE/BE）か。BOM 付きはテキスト確定にする
// （UTF-16 は NUL を多く含むため NUL 判定で誤ってバイナリ扱いになるのを防ぐ）。
bool starts_with_text_bom(std::string_view b) noexcept
{
    const auto u = [&](std::size_t i) { return static_cast<std::uint8_t>(b[i]); };
    if (b.size() >= 3 && u(0) == 0xEF && u(1) == 0xBB && u(2) == 0xBF)
    {
        return true; // UTF-8 BOM
    }
    if (b.size() >= 2 && ((u(0) == 0xFF && u(1) == 0xFE) || (u(0) == 0xFE && u(1) == 0xFF)))
    {
        return true; // UTF-16 LE / BE BOM
    }
    return false;
}

// テキストとして許容する制御文字（タブ・改行・復帰）。それ以外の C0/0x7F は「不審な制御文字」。
bool is_allowed_control(std::uint8_t c) noexcept
{
    return c == 0x09 || c == 0x0A || c == 0x0D;
}

} // namespace

bool looks_binary(std::string_view chunk)
{
    if (chunk.empty())
    {
        return false; // 空はテキスト扱い（空エディタで開く＝従来挙動）。
    }
    if (starts_with_text_bom(chunk))
    {
        return false; // BOM 付きテキストは NUL/制御文字比率に関わらずテキスト確定。
    }

    std::size_t suspicious = 0; // 不審な制御文字の数
    for (char ch : chunk)
    {
        const std::uint8_t c = static_cast<std::uint8_t>(ch);
        if (c == 0x00)
        {
            return true; // NUL を含む＝バイナリ確定（テキストには現れない）。
        }
        if ((c < 0x20 && !is_allowed_control(c)) || c == 0x7F)
        {
            ++suspicious;
        }
    }

    // 不審な制御文字が一定比率（>10%）を超えればバイナリと見なす。テキスト（UTF-8/Shift_JIS）は
    // 高位バイトを含み得るが C0 制御文字はほぼ出ないため、低い閾値でも誤判定は起きにくい。
    return suspicious * 10 > chunk.size();
}

} // namespace pika::util
