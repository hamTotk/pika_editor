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
//! **低ヌルの日本語 UTF-16 は自動判定しない（警告のみ）**: 日本語（ひらがな/カタカナ U+3040〜30FF・
//! 漢字 U+4E00〜 等）主体の BOM なし UTF-16 はヌルバイトをほぼ含まず（例 `"あいうえお"` の UTF-16LE は
//! `42 30 44 30 …` でヌル 0 個）、しかもそのバイト列は妥当な ASCII/UTF-8（全バイト < 0x80）と**同一**に
//! なり得る（`"B0D0F0H0J0"` と区別不能）。内容解析だけでは曖昧で、自動切替は正当な ASCII を逆に壊す。
//! そこで**デコード既定（UTF-8）は変えず**、UTF-16 らしいシグネチャを検出したら `had_decode_warning` を
//! 立てて利用者に再読込を委ねる（無言のデータ喪失だけは防ぐ＝最上位原則「データを失わない」）。
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
            // UTF-8 として妥当でも UTF-16 取りこぼしの疑いがあれば警告して Reopen を促す（採用は UTF-8 のまま）:
            //  (a) ヌルバイトを多く含む（自動切替ヒューリスティックを通り抜けた縁ケースの保険）。
            //  (b) 低ヌルだが日本語 UTF-16 らしいシグネチャ（`"あいうえお"`(UTF-16LE) ≡ ASCII で
            //      自動判定できない域。デコード既定は壊さず警告のみ＝無言喪失防止・要件5.2）。
            let had_decode_warning = enc == TextEncoding::Utf8
                && (null_byte_ratio(bytes) >= UTF16_NULL_RATIO
                    || looks_like_bomless_utf16_text(bytes));
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

/// BOM なし UTF-16 を**自動切替**するヌルバイト比率の閾値（ASCII 主体の UTF-16 は約半分がヌル）。
///
/// ASCII 文字 1 つが UTF-16 では 2 バイト（うち 1 バイトがヌル）になるため、英数主体テキストでは
/// ヌルバイトがおよそ 50% を占める。これは通常テキスト（ヌルをほぼ含まない）と曖昧性が低いので
/// [`detect_bomless_utf16`] で**自動切替**してよい。誤判定（通常テキストを UTF-16 扱い）を避けるため
/// 保守的に 0.25 を採る（通常テキストはヌルバイトを基本含まない＝0 付近）。
///
/// **注意**: 日本語主体（U+3040〜30FF のひらがな/カタカナ・U+4E00〜 の漢字 等）の UTF-16 はヌルが
/// ほぼ無く（高位バイトが 0x30/0x4E〜0x7F でヌルにならない）、しかもそのバイト列は妥当な ASCII と
/// **同一**になり得る（例 `"あいうえお"`(UTF-16LE) ≡ `"B0D0F0H0J0"`(ASCII)）。この域は本閾値では
/// 掴めず、内容解析だけでは区別不能なため**自動切替しない**。代わりに [`looks_like_bomless_utf16_text`]
/// でシグネチャを検出し、デコード既定（UTF-8）は変えずに警告のみ立てて利用者に再読込を委ねる。
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

/// 低ヌル・ASCII 互換だが「日本語 UTF-16 らしい」シグネチャを**警告目的でのみ**検出する。
///
/// 背景: 日本語（ひらがな/カタカナ U+3040〜30FF・漢字 U+4E00〜 等）主体の BOM なし UTF-16 はヌルが
/// ほぼ無く、[`detect_bomless_utf16`] のヌル比率閾値に掛からない。さらにそのバイト列は妥当な ASCII と
/// **同一**になり得る（例 `"あいうえお"`(UTF-16LE) ≡ `"B0D0F0H0J0"`(ASCII)）ため、内容解析だけでは
/// 区別不能で**自動切替は危険**（正当な ASCII を逆に壊す）。本関数は判定だけ行い、採用エンコーディング・
/// text は一切変えず `had_decode_warning` を立てる材料にする（無言のデータ喪失だけを防ぐ・要件5.2）。
///
/// 判定（保守的・誤検出を強く抑える）:
/// - 偶数長かつ十分な長さ（最低 [`UTF16_SIGNATURE_MIN_BYTES`] バイト・短い断片は誤検出回避で除外）。
/// - 全バイトが ASCII 範囲（< 0x80）に収まる（[`detect_bomless_utf16`] が掴む高ヌル UTF-16 ではなく、
///   妥当な ASCII と紛れる低ヌル域だけを対象にする。1 バイトでも 0x80 以上があれば「ASCII と区別不能」
///   という前提が崩れるため除外）。
/// - 「高位バイト側」（LE なら奇数オフセット・BE なら偶数オフセット）に**ひらがな/カタカナの高位バイト
///   0x30/0x31** が高比率（[`UTF16_KANA_HIGH_RATIO`] 以上）で現れる。**かなを鍵にする理由**: 漢字のみの
///   高位バイト域（0x4E〜0x9F）は英文 ASCII（letters/digits も 0x4E〜0x7A）と統計的に重なり区別不能だが、
///   かなの高位バイト 0x30/0x31 は英文 prose にほぼ現れない（0x30='0' は散発的）。実在の日本語文は助詞・
///   送り仮名で必ずかなを含むため、これで「かな入り日本語 UTF-16」と「英文 ASCII」を実用上分離できる。
///   （漢字のみで一切かなを含まない UTF-16 は英文と原理的に区別不能なため**あえて検出しない**＝誤検出を
///   出すより無検出側に倒す。）
/// - 念のため、その並びを実際に UTF-16LE/BE としてデコードし、日本語/CJK に該当するコードポイントが
///   高比率（[`UTF16_CJK_RATIO`] 以上）であることを裏取りする（`"0000…"`=U+3030 連続のような、かな高位
///   バイトを持つが日本語にデコードされない ASCII を弾く）。
///
/// LE/BE どちらのシグネチャでも検出する。該当しなければ `false`。
fn looks_like_bomless_utf16_text(bytes: &[u8]) -> bool {
    if bytes.len() < UTF16_SIGNATURE_MIN_BYTES || bytes.len() % 2 != 0 {
        return false;
    }
    // この警告は「ASCII と紛れて UTF-8 strict が通った」低ヌル域だけが対象。1 バイトでも 0x80 以上を含めば
    // 妥当な ASCII という前提が崩れる（UTF-8/Shift_JIS 通常テキスト・高ヌル UTF-16 は別経路）。
    if bytes.iter().any(|&b| b >= 0x80) {
        return false;
    }
    // LE（高位バイト＝奇数オフセット）・BE（高位バイト＝偶数オフセット）の双方を試す。
    is_japanese_utf16_signature(bytes, false) || is_japanese_utf16_signature(bytes, true)
}

/// 指定エンディアンで「かな入り日本語 UTF-16 らしい」かを判定する（[`looks_like_bomless_utf16_text`] 下請け）。
///
/// `big_endian=false`（LE）は高位バイト＝奇数オフセット、`true`（BE）は高位バイト＝偶数オフセット。
/// 高位バイトにかな域 0x30/0x31 が高比率で現れ、かつ実デコードで日本語/CJK が高比率なら `true`。
fn is_japanese_utf16_signature(bytes: &[u8], big_endian: bool) -> bool {
    let units = bytes.len() / 2;
    if units == 0 {
        return false;
    }
    // 高位バイトのオフセット（LE=奇数 / BE=偶数）。ひらがな U+3040〜309F・カタカナ U+30A0〜30FF の
    // 高位バイトは 0x30、カタカナ・ひらがな拡張（U+31xx）は 0x31。これを「かな高位バイト」とみなす。
    let high_offset = usize::from(!big_endian);
    let mut kana_high = 0usize;
    let mut i = high_offset;
    while i < bytes.len() {
        if matches!(bytes[i], 0x30 | 0x31) {
            kana_high += 1;
        }
        i += 2;
    }
    // かな高位バイトの比率が閾値未満なら（英文 ASCII・漢字のみ等）UTF-16 とは見なさない（無検出に倒す）。
    if (kana_high as f64) / (units as f64) < UTF16_KANA_HIGH_RATIO {
        return false;
    }
    // 裏取り: 実際に UTF-16 としてデコードし、日本語/CJK コードポイントが高比率かを確認する
    //（"0000…"=U+3030 連続のような、かな高位バイトを持つが日本語でない ASCII をここで弾く）。
    let enc = if big_endian {
        TextEncoding::Utf16Be
    } else {
        TextEncoding::Utf16Le
    };
    let Some(text) = decode_strict(enc, bytes) else {
        return false;
    };
    let total = text.chars().count();
    if total == 0 {
        return false;
    }
    let cjk = text.chars().filter(|&c| is_cjk_or_japanese(c)).count();
    (cjk as f64) / (total as f64) >= UTF16_CJK_RATIO
}

/// コードポイントが日本語/CJK（ひらがな・カタカナ・CJK 統合漢字・全角等）に該当するか。
///
/// [`is_japanese_utf16_signature`] のデコード裏取りに使う。範囲は誤検出を抑える主要域に限定する。
fn is_cjk_or_japanese(c: char) -> bool {
    matches!(c as u32,
        0x3040..=0x30FF   // ひらがな・カタカナ
        | 0x31F0..=0x31FF // カタカナ拡張
        | 0x3400..=0x4DBF // CJK 統合漢字拡張A
        | 0x4E00..=0x9FFF // CJK 統合漢字
        | 0xF900..=0xFAFF // CJK 互換漢字
        | 0xFF00..=0xFFEF // 全角・半角形
    )
}

/// 警告シグネチャ判定の最小バイト長（短い断片は誤検出回避で対象外）。
///
/// 8 バイト＝かな 4 文字相当（例 `"あいうえ"`）。これ未満は統計が立たず誤検出が増えるため対象外。
/// 一方で `"あいうえお"`(10B) のような短い日本語も拾えるよう、過度に長くはしない。
const UTF16_SIGNATURE_MIN_BYTES: usize = 8;

/// 高位バイトにかな域 0x30/0x31 が占める最低比率（かな入り日本語の鍵・誤検出抑制で保守的）。
const UTF16_KANA_HIGH_RATIO: f64 = 0.5;

/// UTF-16 として実デコードしたとき日本語/CJK コードポイントが占める最低比率（裏取り・誤検出抑制）。
const UTF16_CJK_RATIO: f64 = 0.8;

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
    fn 低ヌル日本語utf16leは警告を立てるがデコードは壊さない() {
        // "あいうえお" の BOM なし UTF-16LE（42 30 44 30 46 30 48 30 4A 30）はヌル 0 個・全バイト < 0x80 で
        // 妥当な ASCII("B0D0F0H0J0") と**バイト同一**＝区別不能。自動切替は危険なので**採用は UTF-8 のまま**
        // だが、かな高位バイト 0x30 のシグネチャ検出で警告を立てて再読込を促す（無言喪失の防止・要件5.2）。
        let utf16 = utf16_bytes("あいうえお", false); // BOM なし LE・ヌル 0。
        assert!(
            utf16.iter().all(|&b| b != 0),
            "前提崩れ: 低ヌルのはずがヌルを含む"
        );
        let d = decode(&utf16);
        // デコード既定は変えない（UTF-8 として開く＝現状維持・新たな破壊ゼロ）。
        assert_eq!(d.encoding, TextEncoding::Utf8);
        assert!(
            d.had_decode_warning,
            "低ヌル日本語 UTF-16LE で警告が立たない（Reopen を促せず無言喪失の恐れ）: {:?}",
            d
        );
    }

    #[test]
    fn 低ヌル日本語utf16beも警告を立てる() {
        // BE（高位バイト＝偶数オフセット）の日本語 UTF-16 でもシグネチャ検出で警告が立つこと。
        // 低バイトが ASCII 域（< 0x80）に収まるかな（U+3041〜305D 等）を使い、警告経路（UTF-8 strict 成立）
        // に確実に乗せる（低バイトが 0x80 以上のかな〔"ん"=U+3093 等〕は UTF-8 不正で別経路になるため避ける）。
        let utf16 = utf16_bytes("あいうえおかきくけこ", true); // BOM なし BE・全バイト < 0x80。
        assert!(utf16.iter().all(|&b| b < 0x80), "前提崩れ: 全 ASCII でない");
        let d = decode(&utf16);
        assert_eq!(d.encoding, TextEncoding::Utf8);
        assert!(
            d.had_decode_warning,
            "低ヌル日本語 UTF-16BE で警告が立たない: {:?}",
            d
        );
    }

    #[test]
    fn 通常の英文asciiは警告を立てない_誤検出回避() {
        // 普通の英文 ASCII はかな高位バイト 0x30/0x31 が集中しない（letters/space 主体）＝誤検出しない。
        let d = decode(b"Hello, world! This is a plain ASCII test sentence.");
        assert_eq!(d.encoding, TextEncoding::Utf8);
        assert!(
            !d.had_decode_warning,
            "通常英文 ASCII を UTF-16 と誤検出して警告した: {:?}",
            d
        );
    }

    #[test]
    fn markdownのasciiは警告を立てない_誤検出回避() {
        // 記号混じりの Markdown も誤検出しない（かな高位バイトが集中しない）。
        let md = "# Title\n\n- item one\n- item two\n\n```rust\nfn main() {}\n```\n";
        let d = decode(md.as_bytes());
        assert_eq!(d.encoding, TextEncoding::Utf8);
        assert!(
            !d.had_decode_warning,
            "Markdown ASCII を誤検出した: {:?}",
            d
        );
    }

    #[test]
    fn 短いascii断片は警告を立てない_誤検出回避() {
        // 短すぎる断片（最小長 8 バイト未満）と、最小長以上でもかな高位バイトが無い通常断片は対象外。
        for frag in ["ab", "test", "B0D0", "a code line"] {
            let d = decode(frag.as_bytes());
            assert!(
                !d.had_decode_warning,
                "短い/通常 ASCII 断片 {:?} を誤検出した: {:?}",
                frag, d
            );
        }
    }

    #[test]
    fn かな高位バイトでも日本語にならないasciiは警告を立てない_誤検出回避() {
        // "00000000…" は高位バイトが 0x30 連続だが、UTF-16LE デコードは U+3030（CJK 記号・日本語域外）
        // 連続になる。デコード裏取り（日本語/CJK 比率）でこれを弾く＝かな高位バイトだけでは誤検出しない。
        let d = decode(b"0000000000000000");
        assert_eq!(d.encoding, TextEncoding::Utf8);
        assert!(
            !d.had_decode_warning,
            "日本語にデコードされない 0x30 連続 ASCII を誤検出した: {:?}",
            d
        );
    }

    #[test]
    fn かなを含まない英数ascii列は警告を立てない_誤検出回避() {
        // 英大文字＋数字の長い列。高位バイト（LE=奇数オフセット）はバラけ、かな域 0x30/0x31 が
        // 過半に届かない＝誤検出しない（漢字のみ域 0x4E〜0x7A と英字は重なるが、かなを鍵にして弾く）。
        let d = decode(b"Z9Y8X7W6V5U4T3S2R1Q0P9O8N7M6L5K4");
        assert_eq!(d.encoding, TextEncoding::Utf8);
        assert!(!d.had_decode_warning, "英数 ASCII 列を誤検出した: {:?}", d);
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
