// util/binary_detect の検証（F-022・要件12.2 I9）。
// NUL/制御文字比率の素朴 heuristic で非テキスト（バイナリ）を判定する。テキスト（日本語含む）と
// BOM 付きテキストはテキスト扱い、NUL を含む/制御文字過多はバイナリ扱い。
#include "util/binary_detect.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using pika::util::looks_binary;

TEST(BinaryDetectTest, EmptyIsText)
{
    EXPECT_FALSE(looks_binary(std::string_view{}));
}

TEST(BinaryDetectTest, PlainAsciiIsText)
{
    EXPECT_FALSE(looks_binary("Hello, world!\nLine 2\tTabbed\r\n"));
}

TEST(BinaryDetectTest, Utf8JapaneseIsText)
{
    // 日本語（高位バイト）は制御文字ではないためテキスト扱い。
    EXPECT_FALSE(looks_binary("これはテキストです。pika で開けます。"));
}

TEST(BinaryDetectTest, NulByteIsBinary)
{
    std::string s = "abc";
    s.push_back('\0');
    s += "def";
    EXPECT_TRUE(looks_binary(s));
}

TEST(BinaryDetectTest, ManyControlCharsIsBinary)
{
    // C0 制御文字（タブ/改行/復帰以外）が多数＝バイナリ。
    std::string s(100, '\x01');
    EXPECT_TRUE(looks_binary(s));
}

TEST(BinaryDetectTest, FewControlCharsStillText)
{
    // 制御文字が少数（10% 以下）ならテキスト扱い（誤判定を避ける）。
    std::string s(100, 'A');
    s[10] = '\x01'; // 1/101 ≈ 1%
    EXPECT_FALSE(looks_binary(s));
}

TEST(BinaryDetectTest, Utf8BomIsText)
{
    std::string s = "\xEF\xBB\xBF";
    s += "text";
    EXPECT_FALSE(looks_binary(s));
}

TEST(BinaryDetectTest, Utf16LeBomIsText)
{
    // UTF-16 は NUL を多く含むが BOM 付きはテキスト確定（NUL 判定で誤判定しない）。
    std::string s = "\xFF\xFE";
    s += std::string("\x41\x00\x42\x00", 4); // "AB" in UTF-16 LE
    EXPECT_FALSE(looks_binary(s));
}

TEST(BinaryDetectTest, Utf16BeBomIsText)
{
    std::string s = "\xFE\xFF";
    s += std::string("\x00\x41\x00\x42", 4); // "AB" in UTF-16 BE
    EXPECT_FALSE(looks_binary(s));
}

TEST(BinaryDetectTest, PngHeaderIsBinary)
{
    // 画像（PNG シグネチャ）はバイナリ（NUL を含む。画像経路でなくても安全側）。
    const unsigned char sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const std::string s(reinterpret_cast<const char*>(sig), 8);
    EXPECT_TRUE(looks_binary(s));
}

} // namespace
