#include "util/encoding.h"

#include <cstdint>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pika::util
{

namespace
{

constexpr unsigned int kCp932 = 932; // Shift_JIS（Windows コードページ）

// UTF-8 の正当性を検査する（不正な継続バイト・過長符号化・サロゲート・範囲外を弾く）。
// BOM なし候補判定で「これは妥当な UTF-8 か」を決めるために使う。
bool is_valid_utf8(std::string_view s)
{
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* end = p + s.size();
    while (p < end)
    {
        const unsigned char c = *p;
        if (c < 0x80)
        {
            ++p;
            continue;
        }

        int extra = 0;
        std::uint32_t cp = 0;
        unsigned char lower = 0x80;
        unsigned char upper = 0xBF;
        if ((c & 0xE0) == 0xC0)
        {
            extra = 1;
            cp = c & 0x1F;
            if (c < 0xC2) // C0/C1 は過長符号化
            {
                return false;
            }
        }
        else if ((c & 0xF0) == 0xE0)
        {
            extra = 2;
            cp = c & 0x0F;
            if (c == 0xE0)
            {
                lower = 0xA0; // 過長符号化の防止
            }
            if (c == 0xED)
            {
                upper = 0x9F; // サロゲート（U+D800..DFFF）の防止
            }
        }
        else if ((c & 0xF8) == 0xF0)
        {
            extra = 3;
            cp = c & 0x07;
            if (c > 0xF4) // U+10FFFF 超
            {
                return false;
            }
            if (c == 0xF0)
            {
                lower = 0x90;
            }
            if (c == 0xF4)
            {
                upper = 0x8F;
            }
        }
        else
        {
            return false; // 継続バイト単独・5/6 バイト列など
        }

        ++p;
        for (int i = 0; i < extra; ++i)
        {
            if (p >= end)
            {
                return false;
            }
            const unsigned char cc = *p;
            const unsigned char lo = (i == 0) ? lower : 0x80;
            const unsigned char hi = (i == 0) ? upper : 0xBF;
            if (cc < lo || cc > hi)
            {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
            ++p;
        }
        (void)cp;
    }
    return true;
}

// UTF-16（LE/BE）バイト列を UTF-8 へ変換する。バイト数が奇数なら失敗。
Result<std::string> utf16_to_utf8(std::string_view bytes, bool big_endian)
{
    if (bytes.size() % 2 != 0)
    {
        return Result<std::string>::err(ErrorCode::Encoding, "UTF-16 のバイト数が奇数です");
    }
    const auto count = bytes.size() / 2;
    std::wstring wide;
    wide.reserve(count);
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    for (std::size_t i = 0; i < count; ++i)
    {
        const unsigned char b0 = p[i * 2];
        const unsigned char b1 = p[i * 2 + 1];
        const wchar_t unit = big_endian ? static_cast<wchar_t>((b0 << 8) | b1)
                                        : static_cast<wchar_t>((b1 << 8) | b0);
        wide.push_back(unit);
    }

    if (wide.empty())
    {
        return Result<std::string>::ok(std::string{});
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0)
    {
        return Result<std::string>::err(ErrorCode::Encoding, "UTF-16→UTF-8 変換に失敗しました");
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), need,
                          nullptr, nullptr);
    return Result<std::string>::ok(std::move(out));
}

// マルチバイト（CP932 等）→ UTF-8。不正バイト列なら失敗（MB_ERR_INVALID_CHARS）。
Result<std::string> mb_to_utf8(std::string_view bytes, unsigned int codepage)
{
    if (bytes.empty())
    {
        return Result<std::string>::ok(std::string{});
    }
    const int wlen = ::MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (wlen <= 0)
    {
        return Result<std::string>::err(ErrorCode::Encoding,
                                        "デコードに失敗しました（不正なバイト列）");
    }
    std::wstring wide(static_cast<std::size_t>(wlen), L'\0');
    ::MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, bytes.data(),
                          static_cast<int>(bytes.size()), wide.data(), wlen);

    const int need =
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (need <= 0)
    {
        return Result<std::string>::err(ErrorCode::Encoding, "UTF-8 への変換に失敗しました");
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, out.data(), need, nullptr, nullptr);
    return Result<std::string>::ok(std::move(out));
}

// UTF-8 → UTF-16（中間表現）。
Result<std::wstring> utf8_to_wide(std::string_view utf8)
{
    if (utf8.empty())
    {
        return Result<std::wstring>::ok(std::wstring{});
    }
    const int wlen = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0)
    {
        return Result<std::wstring>::err(ErrorCode::Encoding, "UTF-8 が不正です");
    }
    std::wstring wide(static_cast<std::size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                          wide.data(), wlen);
    return Result<std::wstring>::ok(std::move(wide));
}

// UTF-16 → CP932。defaultChar 置換が発生したら（表現不能文字あり）失敗を伝える。
//
// WC_NO_BEST_FIT_CHARS が必須（要件5.2・データを失わない）。これが無いと CP932 に厳密には
// 存在しない一部 Unicode 文字（波ダッシュ U+FF5E→0x8160 等のベストフィット対象）が
// lpUsedDefaultChar を立てずに別バイト列へ静かに化け、表現可能性チェック（!used_default）を
// すり抜けて不可逆破壊する。WC_NO_BEST_FIT_CHARS を付けると未マップ文字は既定文字へ落ち、
// used_default が確実に立つため判定の信頼性が回復する。
Result<std::string> wide_to_cp932(const std::wstring& wide, bool* used_default_out)
{
    if (wide.empty())
    {
        if (used_default_out)
        {
            *used_default_out = false;
        }
        return Result<std::string>::ok(std::string{});
    }
    // サイズ取得時は lpUsedDefaultChar/lpDefaultChar を渡せない（Win32 仕様）。
    const int need =
        ::WideCharToMultiByte(kCp932, WC_NO_BEST_FIT_CHARS, wide.data(),
                              static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (need <= 0)
    {
        return Result<std::string>::err(ErrorCode::Encoding, "Shift_JIS への変換に失敗しました");
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    BOOL used_default = FALSE;
    ::WideCharToMultiByte(kCp932, WC_NO_BEST_FIT_CHARS, wide.data(), static_cast<int>(wide.size()),
                          out.data(), need, nullptr, &used_default);
    if (used_default_out)
    {
        *used_default_out = used_default != FALSE;
    }
    return Result<std::string>::ok(std::move(out));
}

DecodedText make_decoded(std::string content, Encoding enc, bool has_bom)
{
    DecodedText d;
    d.newline = detect_newline(content);
    d.content = std::move(content);
    d.encoding = enc;
    d.has_bom = has_bom;
    return d;
}

} // namespace

Newline detect_newline(std::string_view utf8)
{
    bool has_crlf = false;
    bool has_lone_lf = false;
    for (std::size_t i = 0; i < utf8.size(); ++i)
    {
        if (utf8[i] == '\n')
        {
            if (i > 0 && utf8[i - 1] == '\r')
            {
                has_crlf = true;
            }
            else
            {
                has_lone_lf = true;
            }
        }
    }
    if (has_crlf && has_lone_lf)
    {
        return Newline::Mixed;
    }
    if (has_crlf)
    {
        return Newline::Crlf;
    }
    if (has_lone_lf)
    {
        return Newline::Lf;
    }
    return Newline::None;
}

Result<DecodedText> decode_auto(std::string_view bytes)
{
    // (1) BOM を最優先。
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        return Result<DecodedText>::ok(
            make_decoded(std::string(bytes.substr(3)), Encoding::Utf8Bom, true));
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE)
    {
        auto r = utf16_to_utf8(bytes.substr(2), /*big_endian=*/false);
        if (r.is_err())
        {
            return Result<DecodedText>::err(r.error());
        }
        return Result<DecodedText>::ok(make_decoded(std::move(r).value(), Encoding::Utf16Le, true));
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF)
    {
        auto r = utf16_to_utf8(bytes.substr(2), /*big_endian=*/true);
        if (r.is_err())
        {
            return Result<DecodedText>::err(r.error());
        }
        return Result<DecodedText>::ok(make_decoded(std::move(r).value(), Encoding::Utf16Be, true));
    }

    // (2) BOM なし: UTF-8 → Shift_JIS の順に妥当性を検査。
    if (is_valid_utf8(bytes))
    {
        return Result<DecodedText>::ok(make_decoded(std::string(bytes), Encoding::Utf8, false));
    }
    auto sjis = mb_to_utf8(bytes, kCp932);
    if (sjis.is_ok())
    {
        return Result<DecodedText>::ok(
            make_decoded(std::move(sjis).value(), Encoding::ShiftJis, false));
    }

    // (3) いずれも妥当でなければ UTF-8 として開く（最後の砦・失敗にはしない）。
    //     不正バイトは置換しつつ CP_UTF8 で復元する（lossy）。
    auto lossy = mb_to_utf8(bytes, CP_UTF8);
    std::string content = lossy.is_ok() ? std::move(lossy).value() : std::string(bytes);
    return Result<DecodedText>::ok(make_decoded(std::move(content), Encoding::Utf8, false));
}

Result<DecodedText> decode_as(std::string_view bytes, Encoding encoding)
{
    switch (encoding)
    {
    case Encoding::Utf8:
        return Result<DecodedText>::ok(make_decoded(std::string(bytes), Encoding::Utf8, false));
    case Encoding::Utf8Bom: {
        std::string_view body = bytes;
        bool has_bom = false;
        if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
            static_cast<unsigned char>(body[1]) == 0xBB &&
            static_cast<unsigned char>(body[2]) == 0xBF)
        {
            body = body.substr(3);
            has_bom = true;
        }
        return Result<DecodedText>::ok(make_decoded(std::string(body), Encoding::Utf8Bom, has_bom));
    }
    case Encoding::Utf16Le:
    case Encoding::Utf16Be: {
        const bool be = encoding == Encoding::Utf16Be;
        std::string_view body = bytes;
        bool has_bom = false;
        if (body.size() >= 2)
        {
            const unsigned char b0 = static_cast<unsigned char>(body[0]);
            const unsigned char b1 = static_cast<unsigned char>(body[1]);
            if ((!be && b0 == 0xFF && b1 == 0xFE) || (be && b0 == 0xFE && b1 == 0xFF))
            {
                body = body.substr(2);
                has_bom = true;
            }
        }
        auto r = utf16_to_utf8(body, be);
        if (r.is_err())
        {
            return Result<DecodedText>::err(r.error());
        }
        return Result<DecodedText>::ok(make_decoded(std::move(r).value(), encoding, has_bom));
    }
    case Encoding::ShiftJis: {
        auto r = mb_to_utf8(bytes, kCp932);
        if (r.is_err())
        {
            return Result<DecodedText>::err(r.error());
        }
        return Result<DecodedText>::ok(
            make_decoded(std::move(r).value(), Encoding::ShiftJis, false));
    }
    }
    return Result<DecodedText>::err(ErrorCode::InvalidArgument, "未知のエンコーディングです");
}

bool can_encode(std::string_view utf8_content, Encoding encoding)
{
    // UTF-8/UTF-16 は全 Unicode を表現できるため検査不要。
    if (encoding != Encoding::ShiftJis)
    {
        return true;
    }
    auto wide = utf8_to_wide(utf8_content);
    if (wide.is_err())
    {
        return false;
    }
    bool used_default = false;
    auto r = wide_to_cp932(wide.value(), &used_default);
    return r.is_ok() && !used_default;
}

Result<std::string> encode(std::string_view utf8_content, Encoding encoding, bool with_bom)
{
    switch (encoding)
    {
    case Encoding::Utf8:
    case Encoding::Utf8Bom: {
        std::string out;
        const bool emit_bom = with_bom || encoding == Encoding::Utf8Bom;
        if (emit_bom)
        {
            out.append("\xEF\xBB\xBF");
        }
        out.append(utf8_content);
        return Result<std::string>::ok(std::move(out));
    }
    case Encoding::Utf16Le:
    case Encoding::Utf16Be: {
        auto wide = utf8_to_wide(utf8_content);
        if (wide.is_err())
        {
            return Result<std::string>::err(wide.error());
        }
        const bool be = encoding == Encoding::Utf16Be;
        std::string out;
        if (with_bom)
        {
            out.push_back(be ? '\xFE' : '\xFF');
            out.push_back(be ? '\xFF' : '\xFE');
        }
        for (wchar_t unit : wide.value())
        {
            const auto u = static_cast<std::uint16_t>(unit);
            if (be)
            {
                out.push_back(static_cast<char>((u >> 8) & 0xFF));
                out.push_back(static_cast<char>(u & 0xFF));
            }
            else
            {
                out.push_back(static_cast<char>(u & 0xFF));
                out.push_back(static_cast<char>((u >> 8) & 0xFF));
            }
        }
        return Result<std::string>::ok(std::move(out));
    }
    case Encoding::ShiftJis: {
        auto wide = utf8_to_wide(utf8_content);
        if (wide.is_err())
        {
            return Result<std::string>::err(wide.error());
        }
        bool used_default = false;
        auto r = wide_to_cp932(wide.value(), &used_default);
        if (r.is_err())
        {
            return r;
        }
        if (used_default)
        {
            // 表現不能文字を検出。無確認の不可逆変換・文字欠落を避けるため保存を中断する（要件5.2）。
            return Result<std::string>::err(
                ErrorCode::Encoding,
                "現在のエンコーディング（Shift_JIS）で表現できない文字が含まれています");
        }
        return r;
    }
    }
    return Result<std::string>::err(ErrorCode::InvalidArgument, "未知のエンコーディングです");
}

} // namespace pika::util
