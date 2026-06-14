// pika util: エンコーディング判定・変換と改行コードの記録。
// design.md 3章 util「エンコーディング判定（BOM→候補順デコード妥当性検査）/変換
// （書き出し時の表現可能性チェック付き）」/ 5.2・5.3。要件5.2。
//
// 方針: 文字列は std::string（UTF-8）に統一し、Win32 境界で UTF-16 に変換する（design.md 2章）。
// Shift_JIS（CP932）は Win32 の MultiByteToWideChar / WideCharToMultiByte で往復する。
// 保存時は読み込み時に記録した encoding・BOM・改行をそのまま復元する（勝手に変換しない）。
//
// 往復不変条件（sprint3 自己保存抑制との整合・要件5.2／7章）:
// 本モジュールは「往復不変な文字集合」に限定して保存を許可する設計をとる。CP932 への書き出しでは
// WC_NO_BEST_FIT_CHARS を付け、CP932 に真のマッピングを持たない文字（ベストフィット対象）を
// used_default で検出して encode を中断する（別バイトへ静かに化けさせない）。これにより encode が
// 成功する文字集合は decode→encode でディスクバイトが安定する（bijective）。結果として
// 無編集保存でディスクバイトが変わらず、sprint3 の自己保存抑制（保存後ハッシュ一致が主条件）が
// 外部変更と誤認しない前提が満たされる。原バイトそのものは保持しない（UTF-8 を正本とし保存時に
// 再生成する）が、許可集合が往復不変であるためハッシュ整合は保たれる。
#pragma once

#include "util/result.h"

#include <string>
#include <string_view>

namespace pika::util
{

// 検出・記録するエンコーディング。BOM の有無は has_bom で別に持つ。
enum class Encoding
{
    Utf8,    // BOM なし UTF-8（新規ファイルの既定）
    Utf8Bom, // UTF-8 BOM 付き
    Utf16Le, // UTF-16 LE（BOM 必須）
    Utf16Be, // UTF-16 BE（BOM 必須）
    ShiftJis // Shift_JIS（CP932）
};

// 改行コードの記録。混在（CRLF と LF が混じる AI 出力）は維持する（統一しない。要件5.2）。
enum class Newline
{
    Lf,    // LF のみ
    Crlf,  // CRLF のみ
    Mixed, // CRLF と LF が混在
    None   // 改行を含まない（空・1 行）
};

// 読み込み結果。content は常に UTF-8（改行は原文のまま維持）。
// encoding/has_bom/newline は保存時の往復復元のために記録する。
struct DecodedText
{
    std::string content; // UTF-8。改行コードは変換しない（原文維持）
    Encoding encoding = Encoding::Utf8;
    bool has_bom = false; // BOM を伴っていたか（UTF-8Bom/UTF-16 は true）
    Newline newline = Newline::None;
};

// バイト列の改行コードを判定する（CR 単独は LF 系として扱わず CRLF/LF の有無で判定する）。
Newline detect_newline(std::string_view utf8);

// 自動判定してデコードする。
// 手順（要件5.2）: (1) BOM を最優先（UTF-8 BOM / UTF-16 LE/BE）、(2) BOM なしは UTF-8 → Shift_JIS
// の 順にデコード妥当性を検査、(3) いずれも妥当でなければ UTF-8 として開く（失敗にはしない）。
// 戻り値は常に成功（最後の砦として UTF-8 lossy で開く）。判定不能で UTF-8 に倒した場合も
// DecodedText を返す。
Result<DecodedText> decode_auto(std::string_view bytes);

// 指定エンコーディングで明示的にデコードする（Reopen with Encoding 用。要件5.7）。
// Shift_JIS で不正バイト列なら Encoding エラーを返す。
Result<DecodedText> decode_as(std::string_view bytes, Encoding encoding);

// UTF-8 の content を、指定 encoding・BOM 付与方針でバイト列へエンコードする（保存用）。
// Shift_JIS で表現できない文字（絵文字等）を含む場合は保存を中断し Encoding
// エラーを返す（要件5.2）。 UTF-8/UTF-16 は表現可能性チェック不要（全 Unicode を表現できる）。
// 改行コードは変換しない（content の改行をそのまま出力する。原文維持）。
Result<std::string> encode(std::string_view utf8_content, Encoding encoding, bool with_bom);

// content が指定 encoding で表現可能かを検査する（保存前チェック。表現不能文字を 1 つでも含めば
// false）。 UTF-8/UTF-16 は常に true。
bool can_encode(std::string_view utf8_content, Encoding encoding);

} // namespace pika::util
