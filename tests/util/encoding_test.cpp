// util/encoding のエンコーディング判定・往復・表現可能性チェックの検証（sprint 2）。
// 要件5.2 / design.md 5.2・5.3。BOM 優先判定、BOMなしの UTF-8→Shift_JIS 順、
// UTF-8/BOM/UTF-16/Shift_JIS × LF/CRLF の往復維持、Shift_JIS 表現不能文字の保存中断を観測する。
#include "util/encoding.h"

#include <string>

#include <gtest/gtest.h>

namespace
{

using pika::util::can_encode;
using pika::util::decode_as;
using pika::util::decode_auto;
using pika::util::encode;
using pika::util::Encoding;
using pika::util::ErrorCode;
using pika::util::Newline;

// バイト列を作りやすくするヘルパ（明示的にバイトを並べる）。
std::string bytes(std::initializer_list<unsigned char> bs)
{
    std::string s;
    for (unsigned char b : bs)
    {
        s.push_back(static_cast<char>(b));
    }
    return s;
}

// --- BOM 優先判定 ---

TEST(EncodingDetect, Utf8BomDetectedAndStripped)
{
    const std::string in = bytes({0xEF, 0xBB, 0xBF}) + "hello";
    auto r = decode_auto(in);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().encoding, Encoding::Utf8Bom);
    EXPECT_TRUE(r.value().has_bom);
    EXPECT_EQ(r.value().content, "hello"); // BOM は content から取り除かれる
}

TEST(EncodingDetect, Utf16LeBomDetected)
{
    // "AB" を UTF-16LE BOM 付きで：FF FE 41 00 42 00
    const std::string in = bytes({0xFF, 0xFE, 0x41, 0x00, 0x42, 0x00});
    auto r = decode_auto(in);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().encoding, Encoding::Utf16Le);
    EXPECT_TRUE(r.value().has_bom);
    EXPECT_EQ(r.value().content, "AB");
}

TEST(EncodingDetect, Utf16BeBomDetected)
{
    const std::string in = bytes({0xFE, 0xFF, 0x00, 0x41, 0x00, 0x42});
    auto r = decode_auto(in);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().encoding, Encoding::Utf16Be);
    EXPECT_TRUE(r.value().has_bom);
    EXPECT_EQ(r.value().content, "AB");
}

// --- BOMなし: UTF-8 → Shift_JIS の順で妥当性検査 ---

TEST(EncodingDetect, BomlessValidUtf8IsUtf8)
{
    // 「日本語」(U+65E5..) は妥当な UTF-8。Shift_JIS より先に UTF-8 が採用される。
    const std::string in = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"; // 日本語
    auto r = decode_auto(in);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().encoding, Encoding::Utf8);
    EXPECT_FALSE(r.value().has_bom);
}

TEST(EncodingDetect, BomlessShiftJisFallsBackToSjis)
{
    // 「日本語」を Shift_JIS で：93 FA 96 7B 8C EA。これは妥当な UTF-8 ではないため SJIS
    // と判定される。
    const std::string in = bytes({0x93, 0xFA, 0x96, 0x7B, 0x8C, 0xEA});
    auto r = decode_auto(in);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().encoding, Encoding::ShiftJis);
    // UTF-8 へデコードされた内容は「日本語」になる。
    EXPECT_EQ(r.value().content, std::string("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"));
}

TEST(EncodingDetect, AsciiIsUtf8)
{
    auto r = decode_auto("plain ascii");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().encoding, Encoding::Utf8);
}

// --- 改行コードの記録 ---

TEST(EncodingNewline, DetectsLfCrlfMixedNone)
{
    EXPECT_EQ(decode_auto("a\nb\nc").value().newline, Newline::Lf);
    EXPECT_EQ(decode_auto("a\r\nb\r\nc").value().newline, Newline::Crlf);
    EXPECT_EQ(decode_auto("a\r\nb\nc").value().newline, Newline::Mixed);
    EXPECT_EQ(decode_auto("no newline").value().newline, Newline::None);
}

// --- 往復維持: encoding・BOM・改行が原文どおり復元される ---

struct RoundTripCase
{
    Encoding encoding;
    bool with_bom;
    const char* nl; // "\n" か "\r\n"
    Newline expected_nl;
};

class EncodingRoundTrip : public ::testing::TestWithParam<RoundTripCase>
{
};

TEST_P(EncodingRoundTrip, PreservesEncodingBomNewline)
{
    const RoundTripCase tc = GetParam();
    // 全エンコーディングで表現できる内容（ASCII＋日本語、SJIS でも表現可能）。
    const std::string utf8 = std::string("行1") + tc.nl + "行2" + tc.nl + "末尾";

    // UTF-8 content → バイト列へエンコード（保存相当）。
    auto enc = encode(utf8, tc.encoding, tc.with_bom);
    ASSERT_TRUE(enc.is_ok()) << "encode failed for case";
    const std::string disk = enc.value();

    // 明示エンコーディングでデコード（読み戻し相当）。encoding/BOM/改行が一致する。
    auto dec = decode_as(disk, tc.encoding);
    ASSERT_TRUE(dec.is_ok());
    EXPECT_EQ(dec.value().content, utf8); // content（UTF-8・改行込み）が原文一致
    EXPECT_EQ(dec.value().newline, tc.expected_nl);
    EXPECT_EQ(dec.value().has_bom, tc.with_bom);
    EXPECT_EQ(dec.value().encoding, tc.encoding);

    // 再エンコードでバイト列が安定する（往復で原バイトが維持される）。
    auto enc2 = encode(dec.value().content, tc.encoding, dec.value().has_bom);
    ASSERT_TRUE(enc2.is_ok());
    EXPECT_EQ(enc2.value(), disk);
}

INSTANTIATE_TEST_SUITE_P(
    AllEncodings, EncodingRoundTrip,
    ::testing::Values(RoundTripCase{Encoding::Utf8, false, "\n", Newline::Lf},
                      RoundTripCase{Encoding::Utf8, false, "\r\n", Newline::Crlf},
                      RoundTripCase{Encoding::Utf8Bom, true, "\n", Newline::Lf},
                      RoundTripCase{Encoding::Utf8Bom, true, "\r\n", Newline::Crlf},
                      RoundTripCase{Encoding::Utf16Le, true, "\n", Newline::Lf},
                      RoundTripCase{Encoding::Utf16Le, true, "\r\n", Newline::Crlf},
                      RoundTripCase{Encoding::Utf16Be, true, "\n", Newline::Lf},
                      RoundTripCase{Encoding::Utf16Be, true, "\r\n", Newline::Crlf},
                      RoundTripCase{Encoding::ShiftJis, false, "\n", Newline::Lf},
                      RoundTripCase{Encoding::ShiftJis, false, "\r\n", Newline::Crlf}));

// auto 判定経由の往復（BOM 付き・SJIS が判定→往復で維持されること）。
TEST(EncodingRoundTrip2, AutoDetectThenReEncodeStable)
{
    // SJIS バイト列を auto 判定 → SJIS と分かる → 再エンコードで原バイト一致。
    const std::string sjis = bytes({0x93, 0xFA, 0x96, 0x7B, 0x0D, 0x0A, 0x8C, 0xEA}); // 日本\r\n語
    auto dec = decode_auto(sjis);
    ASSERT_TRUE(dec.is_ok());
    EXPECT_EQ(dec.value().encoding, Encoding::ShiftJis);
    EXPECT_EQ(dec.value().newline, Newline::Crlf);
    auto re = encode(dec.value().content, dec.value().encoding, dec.value().has_bom);
    ASSERT_TRUE(re.is_ok());
    EXPECT_EQ(re.value(), sjis);
}

// --- 表現可能性チェック: Shift_JIS で表現できない文字（絵文字）で保存中断 ---

TEST(EncodingRepresentable, ShiftJisRejectsEmoji)
{
    const std::string emoji = "テスト\xF0\x9F\x98\x80"; // テスト + U+1F600 (grinning face)
    EXPECT_FALSE(can_encode(emoji, Encoding::ShiftJis));

    auto r = encode(emoji, Encoding::ShiftJis, false);
    ASSERT_TRUE(r.is_err()); // 保存中断（例外を投げずエラー値）
    EXPECT_EQ(r.code(), ErrorCode::Encoding);
}

TEST(EncodingRepresentable, ShiftJisAcceptsRepresentable)
{
    const std::string jp = "日本語テスト"; // すべて CP932 にある
    EXPECT_TRUE(can_encode(jp, Encoding::ShiftJis));
    EXPECT_TRUE(encode(jp, Encoding::ShiftJis, false).is_ok());
}

// ベストフィット境界文字: CP932 に真のマッピングを持たない文字が、ベストフィット変換で
// 別バイト列へ静かに化けることなく「表現不能」として検出され、保存が中断されることを観測する。
// WC_NO_BEST_FIT_CHARS 無しではこれらが used_default を立てず誤って「表現可能」と判定され、
// 化けたバイトを保存中断せず書き出す不可逆破壊が起きる（要件5.2・データを失わない）。
//
// 注意（波ダッシュ問題）: Windows CP932 は U+FF5E 全角チルダを 0x8160 に「真に」マップする
// （往復安定・表現可能）。その Unicode 正規対応である U+301C 波ダッシュは Windows CP932 に
// 真のエントリを持たずベストフィット対象になる。本テストは後者のような真に未マップな文字を踏む。
TEST(EncodingRepresentable, ShiftJisRejectsBestFitBoundaryChars)
{
    struct Case
    {
        const char* utf8; // 各 U+xxxx の UTF-8 表現
        const char* name;
    };
    // U+301C 波ダッシュ（WAVE DASH。Windows CP932 に真のエントリなし＝ベストフィット対象）
    // U+2212 マイナス記号（MINUS SIGN。Windows CP932 に真のエントリなし＝ベストフィット対象）
    // U+1F600 絵文字（CP932 に存在しない＝表現不能の対照）
    // ※ U+FF5E 全角チルダ・U+FF0D 全角ハイフンマイナスは Windows CP932 に真のエントリを持ち
    //   表現可能なため、ここには含めない（下の ShiftJisAcceptsTrueCp932Symbols
    //   で表現可能側を観測）。
    const Case cases[] = {
        {"\xE3\x80\x9C", "U+301C WAVE DASH"},
        {"\xE2\x88\x92", "U+2212 MINUS SIGN"},
        {"\xF0\x9F\x98\x80", "U+1F600 GRINNING FACE"},
    };
    for (const auto& c : cases)
    {
        const std::string s = std::string("a") + c.utf8 + "b";
        EXPECT_FALSE(can_encode(s, Encoding::ShiftJis)) << c.name;

        auto r = encode(s, Encoding::ShiftJis, false);
        ASSERT_TRUE(r.is_err()) << c.name
                                << " は保存中断されるべき（ベストフィットで化けさせない）";
        EXPECT_EQ(r.code(), ErrorCode::Encoding) << c.name;
    }
}

// 真に CP932 にある文字（U+FF5E 全角チルダの 0x8160 マッピング等）は表現可能のまま保存でき、
// かつ往復でバイトが安定する（過剰拒否せず・往復不変）ことを観測する。
TEST(EncodingRepresentable, ShiftJisAcceptsTrueCp932Symbols)
{
    // U+3000 全角空白・U+30FC 長音符・U+FF5E 全角チルダ・U+FF01 全角感嘆符は CP932 に存在する。
    const std::string s = "テスト\xE3\x80\x80\xE3\x83\xBC\xEF\xBD\x9E\xEF\xBC\x81";
    EXPECT_TRUE(can_encode(s, Encoding::ShiftJis));
    auto r = encode(s, Encoding::ShiftJis, false);
    ASSERT_TRUE(r.is_ok());

    // CP932 にある文字は往復でバイトが安定する（再デコード→再エンコードで原バイト一致）。
    auto dec = decode_as(r.value(), Encoding::ShiftJis);
    ASSERT_TRUE(dec.is_ok());
    EXPECT_EQ(dec.value().content, s);
    auto re = encode(dec.value().content, Encoding::ShiftJis, false);
    ASSERT_TRUE(re.is_ok());
    EXPECT_EQ(re.value(),
              r.value()); // 往復不変（bijective な文字集合では disk バイトが変わらない）
}

TEST(EncodingRepresentable, Utf8AndUtf16AlwaysRepresentable)
{
    const std::string emoji = "\xF0\x9F\x98\x80"; // 絵文字
    EXPECT_TRUE(can_encode(emoji, Encoding::Utf8));
    EXPECT_TRUE(can_encode(emoji, Encoding::Utf16Le));
    EXPECT_TRUE(encode(emoji, Encoding::Utf8, false).is_ok());
    EXPECT_TRUE(encode(emoji, Encoding::Utf16Le, true).is_ok());
}

// --- 誤判定された UTF-8 を明示 Shift_JIS で開き直す（Reopen with Encoding 相当） ---

TEST(EncodingReopen, DecodeAsOverridesAutoGuess)
{
    // SJIS の「日本語」を、誤って UTF-8 とみなさず明示 SJIS でデコードできる。
    const std::string sjis = bytes({0x93, 0xFA, 0x96, 0x7B, 0x8C, 0xEA});
    auto r = decode_as(sjis, Encoding::ShiftJis);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().content, std::string("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"));
}

} // namespace
