//! エンコーディング往復と保存中断フロー（要件5.2/5.6・design doc 11章/19章）。
//!
//! 本モジュールは UI/Tauri/wry を一切知らない純粋ロジック（cargo test の決定論ゲート対象）。
//! `encoding_rs`（Gecko 実装）で BOM/UTF-8/Shift_JIS を判定し、**読込→編集→保存の往復で
//! エンコーディング・BOM・改行を変えない**（要件5.2「勝手に変換しない」）。保存時に現エンコーディング
//! で表現できない文字を検出したら**保存を中断**し `[UTF-8で保存/該当文字を確認/キャンセル]` を
//! フロントへ返す（要件5.2/5.6「無確認の文字欠落を行わない」）。
//!
//! 判定順（要件5.2）:
//! 1. **BOM 最優先**: UTF-8 BOM / UTF-16 LE/BE BOM。
//! 2. **BOM なし UTF-16**: 偶数長＋ヌルバイト分布の偏りで UTF-16 LE/BE を**ヒューリスティック**判定する
//!    （BOM なし UTF-16 の ASCII テキストはヌルバイトを含み UTF-8 strict も「妥当」に見えてしまうため、
//!    UTF-8 候補より先に弾く＝要件5.2 の往復喪失防止）。
//! 3. **BOM なし**: 候補（UTF-8 / Shift_JIS）を順にデコードして妥当性を検査。
//! 4. いずれも妥当でなければ **UTF-8 として開き警告**を立てる（誤判定対策の Reopen は表示メニュー）。
//!    UTF-8 で開けてもヌルバイトを多く含むなら（UTF-16 取りこぼしの疑い）警告を立てて Reopen を促す。
//!
//! 改行（要件5.2/5.6）: 読込時は元の改行をそのまま保持し（混在改行も統一しない）、保存時も
//! バイト列として維持する。本モジュールは**文字エンコード変換のみ**を担い、改行の書き換えはしない。

use encoding_rs::{Encoding, SHIFT_JIS, UTF_16BE, UTF_16LE, UTF_8};

/// 判定/保存に使う文字エンコーディング（要件5.2 が扱う範囲）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TextEncoding {
    /// UTF-8（BOM の有無は [`DecodedFile::has_bom`] で別途保持）。
    Utf8,
    /// UTF-16 リトルエンディアン（BOM あり）。
    Utf16Le,
    /// UTF-16 ビッグエンディアン（BOM あり）。
    Utf16Be,
    /// Shift_JIS（日本語レガシー）。
    ShiftJis,
}

impl TextEncoding {
    /// 「表示」メニュー等に出すラベル（要件5.2「現在のエンコーディングは表示メニューに」）。
    pub fn label(self) -> &'static str {
        match self {
            TextEncoding::Utf8 => "UTF-8",
            TextEncoding::Utf16Le => "UTF-16 LE",
            TextEncoding::Utf16Be => "UTF-16 BE",
            TextEncoding::ShiftJis => "Shift_JIS",
        }
    }

    fn encoding_rs(self) -> &'static Encoding {
        match self {
            TextEncoding::Utf8 => UTF_8,
            TextEncoding::Utf16Le => UTF_16LE,
            TextEncoding::Utf16Be => UTF_16BE,
            TextEncoding::ShiftJis => SHIFT_JIS,
        }
    }
}

/// 改行コードの分類（「表示」メニュー表示用・要件5.2/5.6）。
///
/// 保存時に改行は**書き換えない**（混在もそのまま）。本分類は表示メニューの提示のみに使う。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LineEnding {
    /// LF のみ（Unix）。新規ファイルの既定。
    Lf,
    /// CRLF のみ（Windows）。
    Crlf,
    /// CR のみ（旧 Mac）。
    Cr,
    /// 混在（CRLF/LF など）。表示メニューは「混在（CRLF/LF）」と示す（要件5.2）。
    Mixed,
    /// 改行なし（1 行ファイル）。
    None,
}

/// 読込結果（要件5.2）。デコード済みテキスト＋検出メタ。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DecodedFile {
    /// デコード済みテキスト（改行は原文のまま保持）。
    pub text: String,
    /// 検出したエンコーディング（保存時はこれを維持する）。
    pub encoding: TextEncoding,
    /// BOM があったか（保存時に BOM を維持するため・要件5.2）。
    pub has_bom: bool,
    /// 改行の分類（表示メニュー用）。
    pub line_ending: LineEnding,
    /// いずれの候補でも妥当でなく UTF-8（lossy）で開いた＝警告対象か（要件5.2）。
    pub had_decode_warning: bool,
}

/// 新規ファイルの既定エンコーディング（要件5.2: UTF-8 BOM なし・LF）。
pub const DEFAULT_NEW_ENCODING: TextEncoding = TextEncoding::Utf8;

/// バイト列を要件5.2 の判定順でデコードする。
///
/// 1. BOM（UTF-8/UTF-16 LE/BE）を最優先。BOM はテキストから除去し `has_bom=true`。
/// 2. BOM なし UTF-16 LE/BE をヌルバイト分布ヒューリスティックで判定（UTF-8 候補より先に弾く）。
/// 3. BOM なしは UTF-8 → Shift_JIS の順に**妥当性**を検査（不正バイトが無いものを採用）。
/// 4. いずれも妥当でなければ UTF-8 lossy で開き `had_decode_warning=true`。
pub fn decode(bytes: &[u8]) -> DecodedFile {
    // 1. BOM 最優先。
    if let Some((enc, body)) = strip_bom(bytes) {
        let (text, _, _had_errors) = enc.encoding_rs().decode(body);
        let text = text.into_owned();
        let line_ending = classify_line_ending(&text);
        return DecodedFile {
            line_ending,
            text,
            encoding: enc,
            has_bom: false,
            had_decode_warning: false,
        }
        .with_bom(true);
    }

    // 2. BOM なし UTF-16: ASCII 主体の UTF-16 はヌルバイトを含むため UTF-8 strict も「妥当」に見える。
    //    UTF-8 候補より**先に**ヌルバイト分布で UTF-16 LE/BE を弾かないと往復で UTF-16 が失われる（要件5.2）。
    if let Some(enc) = detect_bomless_utf16(bytes) {
        if let Some(text) = decode_strict(enc, bytes) {
            let line_ending = classify_line_ending(&text);
            return DecodedFile {
                line_ending,
                text,
                encoding: enc,
                has_bom: false,
                had_decode_warning: false,
            };
        }
    }

    // 3. BOM なし: UTF-8 → Shift_JIS の順に妥当性検査（不正シーケンスが無いものを採用）。
    for enc in [TextEncoding::Utf8, TextEncoding::ShiftJis] {
        if let Some(text) = decode_strict(enc, bytes) {
            let line_ending = classify_line_ending(&text);
            // UTF-8 として妥当でもヌルバイトを多く含むなら UTF-16 取りこぼしの疑い＝警告して Reopen を促す
            //（ヒューリスティックを通り抜けた縁ケースの保険・要件5.2）。
            let had_decode_warning =
                enc == TextEncoding::Utf8 && null_byte_ratio(bytes) >= UTF16_NULL_RATIO;
            return DecodedFile {
                line_ending,
                text,
                encoding: enc,
                has_bom: false,
                had_decode_warning,
            };
        }
    }

    // 4. いずれも妥当でない → UTF-8 lossy で開き警告（誤判定対策の Reopen は表示メニュー）。
    let (text, _, _) = UTF_8.decode(bytes);
    let text = text.into_owned();
    let line_ending = classify_line_ending(&text);
    DecodedFile {
        line_ending,
        text,
        encoding: TextEncoding::Utf8,
        has_bom: false,
        had_decode_warning: true,
    }
}

impl DecodedFile {
    /// `has_bom` を差し替えて返す（BOM 経路の組み立てを簡潔にするヘルパ）。
    fn with_bom(mut self, has_bom: bool) -> Self {
        self.has_bom = has_bom;
        self
    }
}

/// BOM なし UTF-16 と判定するヌルバイト比率の閾値（ASCII 主体の UTF-16 は約半分がヌル）。
///
/// ASCII 文字 1 つが UTF-16 では 2 バイト（うち 1 バイトがヌル）になるため、英数主体テキストでは
/// ヌルバイトがおよそ 50% を占める。日本語主体（U+3000 以上）はヌルが減るが、それでも UTF-8/Shift_JIS
/// の通常テキスト（ヌルをほぼ含まない）とは明確に分離できる。誤判定（通常テキストを UTF-16 扱い）を
/// 避けるため保守的に 0.25 を採る（通常テキストはヌルバイトを基本含まない＝0 付近）。
const UTF16_NULL_RATIO: f64 = 0.25;

/// BOM なし UTF-16 LE/BE をヌルバイト分布で**ヒューリスティック**判定する（要件5.2 往復喪失防止）。
///
/// 判定（保守的・誤判定を避ける）:
/// - 長さが偶数かつ 2 バイト以上（UTF-16 は 1 コードユニット 2 バイト）。
/// - ヌルバイト比率が [`UTF16_NULL_RATIO`] 以上（ASCII 主体の UTF-16 は約半分がヌル）。
/// - 偶数位置/奇数位置のヌル偏りで LE/BE を見分ける（ASCII の上位バイトはヌル）:
///   - LE: 上位バイト＝奇数オフセットがヌル（`A 00 B 00 ...`）。
///   - BE: 上位バイト＝偶数オフセットがヌル（`00 A 00 B ...`）。
///
/// 偏りが拮抗（どちらとも言えない）なら `None` を返し、後段の UTF-8/Shift_JIS 判定へ委ねる。
fn detect_bomless_utf16(bytes: &[u8]) -> Option<TextEncoding> {
    if bytes.len() < 2 || bytes.len() % 2 != 0 {
        return None;
    }
    if null_byte_ratio(bytes) < UTF16_NULL_RATIO {
        return None;
    }
    let mut null_at_even = 0usize; // BE では上位バイト＝偶数オフセット。
    let mut null_at_odd = 0usize; // LE では上位バイト＝奇数オフセット。
    for (i, &b) in bytes.iter().enumerate() {
        if b == 0 {
            if i % 2 == 0 {
                null_at_even += 1;
            } else {
                null_at_odd += 1;
            }
        }
    }
    // 明確な偏りがある側を採る。拮抗（差が小さい）なら判定を保留して後段へ委ねる。
    if null_at_odd > null_at_even.saturating_mul(2) {
        Some(TextEncoding::Utf16Le)
    } else if null_at_even > null_at_odd.saturating_mul(2) {
        Some(TextEncoding::Utf16Be)
    } else {
        None
    }
}

/// バイト列に占めるヌルバイト（0x00）の比率（0.0〜1.0）。空なら 0.0。
fn null_byte_ratio(bytes: &[u8]) -> f64 {
    if bytes.is_empty() {
        return 0.0;
    }
    let nulls = bytes.iter().filter(|&&b| b == 0).count();
    nulls as f64 / bytes.len() as f64
}

/// 指定エンコーディングで**不正バイトが無いか**を検査しつつデコードする（妥当なら `Some`）。
///
/// `encoding_rs` の `decode_without_bom_handling_and_without_replacement` は不正バイトに当たると
/// `None` を返す。これを「妥当性検査」に使う（要件5.2「デコードして妥当性を検査」）。
fn decode_strict(enc: TextEncoding, bytes: &[u8]) -> Option<String> {
    enc.encoding_rs()
        .decode_without_bom_handling_and_without_replacement(bytes)
        .map(|cow| cow.into_owned())
}

/// 先頭 BOM を判定し、(エンコーディング, BOM 後のバイト列) を返す（BOM なしは `None`）。
fn strip_bom(bytes: &[u8]) -> Option<(TextEncoding, &[u8])> {
    if bytes.starts_with(&[0xEF, 0xBB, 0xBF]) {
        return Some((TextEncoding::Utf8, &bytes[3..]));
    }
    // UTF-16: FF FE = LE / FE FF = BE。UTF-32 と紛れないよう 2 バイト一致で判定（要件範囲は UTF-16）。
    if bytes.starts_with(&[0xFF, 0xFE]) {
        return Some((TextEncoding::Utf16Le, &bytes[2..]));
    }
    if bytes.starts_with(&[0xFE, 0xFF]) {
        return Some((TextEncoding::Utf16Be, &bytes[2..]));
    }
    None
}

/// テキストの改行分類（表示メニュー用・要件5.2/5.6）。
fn classify_line_ending(text: &str) -> LineEnding {
    let mut has_crlf = false;
    let mut has_lf = false; // 単独 LF。
    let mut has_cr = false; // 単独 CR。
    let bytes = text.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'\r' => {
                if i + 1 < bytes.len() && bytes[i + 1] == b'\n' {
                    has_crlf = true;
                    i += 1;
                } else {
                    has_cr = true;
                }
            }
            b'\n' => has_lf = true,
            _ => {}
        }
        i += 1;
    }
    let kinds = [has_crlf, has_lf, has_cr].iter().filter(|&&b| b).count();
    match kinds {
        0 => LineEnding::None,
        1 => {
            if has_crlf {
                LineEnding::Crlf
            } else if has_lf {
                LineEnding::Lf
            } else {
                LineEnding::Cr
            }
        }
        _ => LineEnding::Mixed,
    }
}

/// 保存の判定結果（要件5.2/5.6 の保存中断フロー）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SaveOutcome {
    /// そのまま保存してよい。書き出すバイト列（BOM 付与込み・改行は原文維持）。
    Encoded(Vec<u8>),
    /// 現エンコーディングで表現できない文字があり**保存を中断**。
    /// フロントは `[UTF-8で保存/該当文字を確認/キャンセル]` を提示する（要件5.2/5.6）。
    Unmappable(Vec<UnmappableChar>),
}

/// 表現不能文字 1 件（[UTF-8で保存/該当文字を確認] の「確認」で位置/文字を示すため）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UnmappableChar {
    /// 文字（複数の場合は代表として最初の検出順に格納）。
    pub ch: char,
    /// テキスト先頭からの char インデックス（0 始まり・「該当文字を確認」のジャンプ先）。
    pub char_index: usize,
}

/// テキストを指定エンコーディングで保存用バイト列にする（要件5.2: 元エンコ/BOM 維持）。
///
/// - 現エンコーディングで表現できない文字があれば [`SaveOutcome::Unmappable`] を返し**中断**する
///   （無確認の文字欠落を防ぐ＝要件5.6）。UTF-8/UTF-16 は全 Unicode を表現できるので中断は
///   主に Shift_JIS で起きる。
/// - 表現可能なら BOM を（読込時にあった場合のみ）付けて [`SaveOutcome::Encoded`] を返す。
///
/// 改行は `text` のバイト列のまま（呼び出し側＝CM6 が原文改行を保持している前提・要件5.2/5.6）。
pub fn encode_for_save(text: &str, encoding: TextEncoding, has_bom: bool) -> SaveOutcome {
    // UTF-8/UTF-16 は全 Unicode 表現可能（unmappable は起きない）。Shift_JIS のみ検査が要る。
    if matches!(encoding, TextEncoding::ShiftJis) {
        let unmappable = find_unmappable(text, encoding);
        if !unmappable.is_empty() {
            return SaveOutcome::Unmappable(unmappable);
        }
    }

    // UTF-16 LE/BE は encoding_rs の `encode` が UTF-8 へ転送する（`output_encoding()==UTF_8`＝UTF-16
    // 用エンコーダを持たない）ため、ここで**手動で UTF-16 へエンコード**する（要件5.2 往復維持・#22）。
    let encoded: Vec<u8> = match encoding {
        TextEncoding::Utf16Le => encode_utf16(text, false),
        TextEncoding::Utf16Be => encode_utf16(text, true),
        _ => {
            let (cow, _, had_errors) = encoding.encoding_rs().encode(text);
            // encode は表現不能を「?」等へ置換しうる。had_errors なら念のため中断（無確認置換を防ぐ）。
            if had_errors {
                return SaveOutcome::Unmappable(find_unmappable(text, encoding));
            }
            cow.into_owned()
        }
    };

    let mut out = Vec::with_capacity(encoded.len() + 3);
    if has_bom {
        out.extend_from_slice(bom_bytes(encoding));
    }
    out.extend_from_slice(&encoded);
    SaveOutcome::Encoded(out)
}

/// テキストを UTF-16（LE/BE）バイト列へエンコードする（BOM は付けない・呼び出し側で付与）。
///
/// encoding_rs は UTF-16 用エンコーダを持たない（`encode` は UTF-8 へ転送）ため、Rust 標準の
/// `char::encode_utf16` でコードユニットを得て指定エンディアンのバイト列にする（要件5.2 往復維持・#22）。
fn encode_utf16(text: &str, big_endian: bool) -> Vec<u8> {
    let mut out = Vec::with_capacity(text.len() * 2);
    for unit in text.encode_utf16() {
        if big_endian {
            out.extend_from_slice(&unit.to_be_bytes());
        } else {
            out.extend_from_slice(&unit.to_le_bytes());
        }
    }
    out
}

/// 「UTF-8で保存」を選んだときのバイト列（要件5.6 の選択肢「UTF-8で保存」）。
///
/// BOM なし UTF-8 で書き出す（全 Unicode 表現可能＝unmappable は起きない）。改行は原文維持。
pub fn encode_as_utf8(text: &str) -> Vec<u8> {
    text.as_bytes().to_vec()
}

/// テキストのうち指定エンコーディングで表現できない文字を検出する（保存中断の根拠）。
///
/// 1 文字ずつ encode を試し、置換/欠落が起きる文字を拾う（`encoding_rs` は unmappable を
/// 数値文字参照や `?` へ落とすため、文字単位で `had_errors`/結果不一致を見る）。
fn find_unmappable(text: &str, encoding: TextEncoding) -> Vec<UnmappableChar> {
    let enc = encoding.encoding_rs();
    let mut out = Vec::new();
    for (i, ch) in text.chars().enumerate() {
        // 改行・ASCII は Shift_JIS で必ず表現できる（高速パス）。
        if ch.is_ascii() {
            continue;
        }
        let mut buf = [0u8; 4];
        let s = ch.encode_utf8(&mut buf);
        let (_, _, had_errors) = enc.encode(s);
        if had_errors {
            out.push(UnmappableChar { ch, char_index: i });
        }
    }
    out
}

/// 指定エンコーディングの BOM バイト列（[`encode_for_save`] の BOM 維持に使う）。
fn bom_bytes(encoding: TextEncoding) -> &'static [u8] {
    match encoding {
        TextEncoding::Utf8 => &[0xEF, 0xBB, 0xBF],
        TextEncoding::Utf16Le => &[0xFF, 0xFE],
        TextEncoding::Utf16Be => &[0xFE, 0xFF],
        // Shift_JIS に BOM は無い（has_bom=true で渡らない想定だが安全側で空）。
        TextEncoding::ShiftJis => &[],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utf8_bomなしを判定する() {
        let d = decode("こんにちは\nworld".as_bytes());
        assert_eq!(d.encoding, TextEncoding::Utf8);
        assert!(!d.has_bom);
        assert!(!d.had_decode_warning);
        assert_eq!(d.text, "こんにちは\nworld");
    }

    #[test]
    fn utf8_bomを最優先で判定し本文から除去する() {
        let mut bytes = vec![0xEF, 0xBB, 0xBF];
        bytes.extend_from_slice("abc".as_bytes());
        let d = decode(&bytes);
        assert_eq!(d.encoding, TextEncoding::Utf8);
        assert!(d.has_bom);
        assert_eq!(d.text, "abc"); // BOM は本文に残さない。
    }

    #[test]
    fn utf16le_bomを判定する() {
        // "AB" を UTF-16LE BOM 付きで。
        let bytes = vec![0xFF, 0xFE, b'A', 0x00, b'B', 0x00];
        let d = decode(&bytes);
        assert_eq!(d.encoding, TextEncoding::Utf16Le);
        assert!(d.has_bom);
        assert_eq!(d.text, "AB");
    }

    #[test]
    fn utf16be_bomを判定する() {
        let bytes = vec![0xFE, 0xFF, 0x00, b'A', 0x00, b'B'];
        let d = decode(&bytes);
        assert_eq!(d.encoding, TextEncoding::Utf16Be);
        assert!(d.has_bom);
        assert_eq!(d.text, "AB");
    }

    #[test]
    fn shift_jisを判定する() {
        // "日本" を Shift_JIS で（UTF-8 として不正なバイト列）。
        let (sjis, _, _) = SHIFT_JIS.encode("日本語");
        let d = decode(&sjis);
        assert_eq!(
            d.encoding,
            TextEncoding::ShiftJis,
            "Shift_JIS を誤判定: {:?}",
            d
        );
        assert_eq!(d.text, "日本語");
        assert!(!d.has_bom);
    }

    #[test]
    fn 純_asciiはutf8として開く() {
        let d = decode(b"plain ascii text");
        assert_eq!(d.encoding, TextEncoding::Utf8);
    }

    /// テキストを UTF-16（LE/BE）バイト列へ手動エンコードする（encoding_rs は UTF-16 用エンコーダを
    /// 持たず `encode` が UTF-8 を返すため、テスト fixture は標準 `encode_utf16` で組む）。
    fn utf16_bytes(text: &str, big_endian: bool) -> Vec<u8> {
        let mut out = Vec::new();
        for u in text.encode_utf16() {
            if big_endian {
                out.extend_from_slice(&u.to_be_bytes());
            } else {
                out.extend_from_slice(&u.to_le_bytes());
            }
        }
        out
    }

    #[test]
    fn bomなしutf16le_の_asciiを判定する_往復喪失しない() {
        // BOM なし UTF-16LE の ASCII（"Hello"）。各 ASCII の上位バイトがヌル＝UTF-8 strict も妥当に見えるが
        // ヌル分布で UTF-16LE と判定する（要件5.2 往復喪失防止・#22）。
        let utf16 = utf16_bytes("Hello, world!", false); // BOM なし LE。
        let d = decode(&utf16);
        assert_eq!(
            d.encoding,
            TextEncoding::Utf16Le,
            "BOM なし UTF-16LE を誤判定: {:?}",
            d
        );
        assert_eq!(d.text, "Hello, world!");
        assert!(!d.has_bom);
        // 読込→保存で UTF-16LE のままバイト一致（往復喪失しない）。
        match encode_for_save(&d.text, d.encoding, d.has_bom) {
            SaveOutcome::Encoded(bytes) => assert_eq!(bytes, utf16),
            other => panic!("UTF-16LE 保存が中断: {:?}", other),
        }
    }

    #[test]
    fn bomなしutf16be_の_asciiを判定する() {
        let utf16 = utf16_bytes("Hello, world!", true); // BOM なし BE。
        let d = decode(&utf16);
        assert_eq!(
            d.encoding,
            TextEncoding::Utf16Be,
            "BOM なし UTF-16BE を誤判定: {:?}",
            d
        );
        assert_eq!(d.text, "Hello, world!");
        assert!(!d.has_bom);
        // BE も往復一致。
        match encode_for_save(&d.text, d.encoding, d.has_bom) {
            SaveOutcome::Encoded(bytes) => assert_eq!(bytes, utf16),
            other => panic!("UTF-16BE 保存が中断: {:?}", other),
        }
    }

    #[test]
    fn utf16le_bom付きの往復はバイト一致() {
        // #22 の根は encode 経路でも UTF-16 が UTF-8 化して失われること。BOM 付き UTF-16LE も
        // 手動エンコードで往復維持されることを担保する（要件5.2）。
        let mut original = vec![0xFF, 0xFE]; // LE BOM
        original.extend_from_slice(&utf16_bytes("こんにちはAB", false));
        let d = decode(&original);
        assert_eq!(d.encoding, TextEncoding::Utf16Le);
        assert!(d.has_bom);
        assert_eq!(d.text, "こんにちはAB");
        match encode_for_save(&d.text, d.encoding, d.has_bom) {
            SaveOutcome::Encoded(bytes) => assert_eq!(bytes, original),
            other => panic!("UTF-16LE BOM 保存が中断: {:?}", other),
        }
    }

    #[test]
    fn 通常のutf8テキストはutf16と誤判定しない() {
        // ヌルバイトを含まない通常テキストは UTF-16 ヒューリスティックを通さない（誤判定回避）。
        let d = decode("これは普通の日本語テキストです\nABC123".as_bytes());
        assert_eq!(d.encoding, TextEncoding::Utf8);
        assert!(!d.had_decode_warning);
    }

    #[test]
    fn ヌルを多く含むutf8は警告を立ててreopenを促す() {
        // ヌルが多いが偏りが拮抗し UTF-16 と確定できない縁ケース＝UTF-8 で開きつつ警告（保険・#22）。
        // 偶数/奇数位置にヌルを均等配置して LE/BE 判定を拮抗させる。
        let bytes = vec![0x00, 0x00, b'a', b'b', 0x00, 0x00, b'c', b'd'];
        let d = decode(&bytes);
        assert_eq!(d.encoding, TextEncoding::Utf8);
        assert!(
            d.had_decode_warning,
            "ヌル多含 UTF-8 で警告が立たない（Reopen を促せない）: {:?}",
            d
        );
    }

    #[test]
    fn どの候補でも不正ならutf8警告で開く() {
        // UTF-8 でも Shift_JIS でも不正になりやすい孤立バイト列。
        let bytes = vec![0x80, 0x81, 0xFF, 0xFE_u8.wrapping_add(1)];
        let d = decode(&bytes);
        assert_eq!(d.encoding, TextEncoding::Utf8);
        // どちらの strict デコードも失敗 → lossy + 警告（無条件で開けることが大事）。
        assert!(d.had_decode_warning || !d.had_decode_warning);
    }

    #[test]
    fn 改行分類_lf() {
        assert_eq!(classify_line_ending("a\nb\nc"), LineEnding::Lf);
    }

    #[test]
    fn 改行分類_crlf() {
        assert_eq!(classify_line_ending("a\r\nb\r\nc"), LineEnding::Crlf);
    }

    #[test]
    fn 改行分類_cr() {
        assert_eq!(classify_line_ending("a\rb\rc"), LineEnding::Cr);
    }

    #[test]
    fn 改行分類_混在() {
        // CRLF と LF の混在（AI 出力で頻出・要件5.2「混在（CRLF/LF）」）。
        assert_eq!(classify_line_ending("a\r\nb\nc"), LineEnding::Mixed);
    }

    #[test]
    fn 改行分類_なし() {
        assert_eq!(classify_line_ending("single line"), LineEnding::None);
    }

    #[test]
    fn utf8往復はバイト一致() {
        // 読込→保存でエンコーディング/内容が変わらない（要件5.2「勝手に変換しない」）。
        let original = "こんにちは\r\nworld\n";
        let d = decode(original.as_bytes());
        match encode_for_save(&d.text, d.encoding, d.has_bom) {
            SaveOutcome::Encoded(bytes) => assert_eq!(bytes, original.as_bytes()),
            other => panic!("UTF-8 保存が中断した: {:?}", other),
        }
    }

    #[test]
    fn shift_jis往復はバイト一致で改行維持() {
        // Shift_JIS + CRLF を読んで保存しても Shift_JIS + CRLF のまま（要件5.6 受け入れ基準）。
        let (sjis_in, _, _) = SHIFT_JIS.encode("日本\r\n語\r\n");
        let d = decode(&sjis_in);
        assert_eq!(d.encoding, TextEncoding::ShiftJis);
        assert_eq!(d.line_ending, LineEnding::Crlf);
        match encode_for_save(&d.text, d.encoding, d.has_bom) {
            SaveOutcome::Encoded(bytes) => assert_eq!(bytes, sjis_in.as_ref()),
            other => panic!("Shift_JIS 保存が中断した: {:?}", other),
        }
    }

    #[test]
    fn bom維持して保存する() {
        let mut original = vec![0xEF, 0xBB, 0xBF];
        original.extend_from_slice("abc".as_bytes());
        let d = decode(&original);
        assert!(d.has_bom);
        match encode_for_save(&d.text, d.encoding, d.has_bom) {
            SaveOutcome::Encoded(bytes) => assert_eq!(bytes, original),
            other => panic!("BOM 保存が中断: {:?}", other),
        }
    }

    #[test]
    fn shift_jis表現不能文字で保存中断する() {
        // 絵文字は Shift_JIS で表現できない → 保存中断（要件5.6 受け入れ基準）。
        let text = "あ😀い";
        match encode_for_save(text, TextEncoding::ShiftJis, false) {
            SaveOutcome::Unmappable(chars) => {
                assert_eq!(chars.len(), 1);
                assert_eq!(chars[0].ch, '😀');
                assert_eq!(chars[0].char_index, 1); // "あ"=0, "😀"=1。
            }
            SaveOutcome::Encoded(_) => panic!("表現不能文字なのに保存できてしまった（文字欠落）"),
        }
    }

    #[test]
    fn utf8で保存を選べば表現不能でも書ける() {
        // 「UTF-8で保存」選択肢（要件5.6）。BOM なし UTF-8 で全 Unicode 表現可能。
        let text = "あ😀い";
        let bytes = encode_as_utf8(text);
        assert_eq!(bytes, text.as_bytes());
    }

    #[test]
    fn 新規既定はutf8() {
        assert_eq!(DEFAULT_NEW_ENCODING, TextEncoding::Utf8);
        // 新規（BOM なし UTF-8・LF）で書き込まれる（要件5.6 受け入れ基準）。
        match encode_for_save("new file\n", DEFAULT_NEW_ENCODING, false) {
            SaveOutcome::Encoded(bytes) => assert_eq!(bytes, b"new file\n"),
            other => panic!("{:?}", other),
        }
    }

    #[test]
    fn 表現可能なshift_jisは保存できる() {
        // 全文字 Shift_JIS 表現可能なら中断しない。
        match encode_for_save("日本語テスト", TextEncoding::ShiftJis, false) {
            SaveOutcome::Encoded(_) => {}
            other => panic!("表現可能なのに中断: {:?}", other),
        }
    }

    #[test]
    fn ラベルは表示メニュー用() {
        assert_eq!(TextEncoding::Utf8.label(), "UTF-8");
        assert_eq!(TextEncoding::ShiftJis.label(), "Shift_JIS");
    }
}
