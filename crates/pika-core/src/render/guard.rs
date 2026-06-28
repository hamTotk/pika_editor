//! レンダリング暴走ガード（要件2.2・design doc 6章/7章「暴走ガードは Rust render 段で計測」）。
//!
//! AI 出力（非定型）を主対象とするため、プレビュー/画像のリソース消費上限を**入力段で計測**し、
//! 超過は配信せず通知バーで「既定のブラウザ/アプリで開く」へ誘導する（WebView 任せにしない）。
//! 既定上限（要件2.2）:
//! - 画像: 総ピクセル数 6000万px
//! - SVG: 展開ピクセル数相当 8000万px / 要素数 5万
//! - HTML: レンダリングタイムアウト 10 秒（タイムアウト値の供給のみ。実計測は描画側）
//!
//! 本モジュールは Tauri/wry/画像デコーダを一切知らない純粋ロジック（cargo test の決定論ゲート対象）。
//! 画像の寸法（width/height）・SVG の要素数/推定ピクセル数は呼び出し側がヘッダ/パースから供給する。

/// 画像の総ピクセル数の既定上限（要件2.2: 6000万px）／1 行の長さガード（要件2.2: 10万字）。
///
/// 正準定義は中立な最下層 [`crate::limits`] にあり（編集系 [`crate::huge`]・非テキスト判定
/// [`crate::nontext`] も参照するため、描画系に置くと「編集系 → 描画系」の上向き依存になる）、ここでは
/// **再エクスポートのみ**行う。これにより既存参照点 `render::guard::DEFAULT_IMAGE_MAX_PIXELS` /
/// `DEFAULT_LONG_LINE_CHARS`（src-tauri は `pika_core::render` 経由で import）は不変に解決できる。
pub use crate::limits::{DEFAULT_IMAGE_MAX_PIXELS, DEFAULT_LONG_LINE_CHARS};

/// SVG の展開ピクセル数相当の既定上限（要件2.2: 8000万px）。
pub const DEFAULT_SVG_MAX_PIXELS: u64 = 80_000_000;
/// SVG の要素数の既定上限（要件2.2: 5万要素）。
pub const DEFAULT_SVG_MAX_ELEMENTS: u64 = 50_000;
/// HTML レンダリングのタイムアウト（要件2.2: 10 秒）。描画側へ供給する値。
pub const DEFAULT_HTML_TIMEOUT_MS: u64 = 10_000;

/// 暴走ガードの判定結果（要件2.2）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum GuardDecision {
    /// 上限内＝配信/デコードしてよい。
    Allow,
    /// 上限超過＝配信せず通知バーで「既定のアプリ/ブラウザで開く」へ誘導する（理由付き）。
    Block(BlockReason),
}

/// 暴走ガードのブロック理由（通知バー文言の根拠＝要件2.2）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BlockReason {
    /// 画像の総ピクセル数が上限超過。
    ImagePixels { pixels: u64, limit: u64 },
    /// SVG の推定ピクセル数が上限超過。
    SvgPixels { pixels: u64, limit: u64 },
    /// SVG の要素数が上限超過。
    SvgElements { elements: u64, limit: u64 },
    /// 画像ヘッダから寸法を判定できなかった（壊れ/細工/未知形式）。
    /// 配信前ガードは fail-closed でブロックし「既定アプリで開く」へ誘導する（#24・nontext と統一）。
    ImageDimensionsUnknown,
}

impl GuardDecision {
    /// 配信/デコードを許可する判定か。
    pub fn is_allowed(&self) -> bool {
        matches!(self, GuardDecision::Allow)
    }
}

/// 画像の総ピクセル数ガード（要件2.2・12.2）。`width*height` が上限を**超える**とブロック。
///
/// デコード前にヘッダから寸法を取得して呼ぶ（巨大画像のデコード爆発で固まらないため）。
pub fn check_image_pixels(width: u64, height: u64, limit: u64) -> GuardDecision {
    let pixels = width.saturating_mul(height);
    if pixels > limit {
        GuardDecision::Block(BlockReason::ImagePixels { pixels, limit })
    } else {
        GuardDecision::Allow
    }
}

/// SVG ガード（要件2.2）。推定ピクセル数または要素数のいずれかが上限超過でブロック。
///
/// - `est_pixels`: `width*height`（viewBox/属性から呼び出し側が推定）。
/// - `element_count`: SVG パースで数えた要素数。
pub fn check_svg(
    est_pixels: u64,
    element_count: u64,
    pixel_limit: u64,
    element_limit: u64,
) -> GuardDecision {
    if est_pixels > pixel_limit {
        return GuardDecision::Block(BlockReason::SvgPixels {
            pixels: est_pixels,
            limit: pixel_limit,
        });
    }
    if element_count > element_limit {
        return GuardDecision::Block(BlockReason::SvgElements {
            elements: element_count,
            limit: element_limit,
        });
    }
    GuardDecision::Allow
}

/// 1 行でも長行ガードに掛かるか（要件2.2: 1行10万字超でハイライト/折返し自動オフ）。
///
/// プレビュー/エディタのハイライト・折返しを無効化すべきかの判定に使う（自動オフ＝固まらない）。
pub fn has_long_line(text: &str, limit_chars: usize) -> bool {
    text.lines().any(|line| line.chars().count() > limit_chars)
}

/// 画像バイト列の**ヘッダから**寸法を読み、デコード前に暴走ガードを判定する（要件2.2・12.2）。
///
/// custom protocol がローカル画像を別WebView へ配信する直前に呼ぶ。巨大画像を WebView に渡すと
/// デコードでメモリ/CPU が爆発し UI が固まる（設計原則「固まらない」違反）ため、**配信前に**
/// ヘッダの width/height だけ読んで [`check_image_pixels`] で弾く（フルデコードしない）。
///
/// 対応フォーマット（ヘッダ寸法を取れるもの）: PNG / GIF / JPEG / BMP / WebP(VP8/VP8L/VP8X) / ICO。
///
/// **寸法不明時の既定（#24・fail-closed）**: ヘッダから width/height を判定できない（壊れ/細工/未知形式）
/// 場合は [`GuardDecision::Block`]（[`BlockReason::ImageDimensionsUnknown`]）でブロックし「既定アプリで開く」へ
/// 誘導する。配信前ガードは「寸法不明＝デコード爆発の危険を否定できない」として**危険側でなく安全側
/// （ブロック寄り）に倒す**。これは [`crate::nontext::decide_image_open`] の寸法不明時方針（外部誘導）と統一する
/// （以前は guard=Allow（危険側）/nontext=外部誘導（安全側）で割れていた）。
pub fn check_image_bytes(bytes: &[u8], pixel_limit: u64) -> GuardDecision {
    match image_dimensions(bytes) {
        Some((w, h)) => check_image_pixels(w, h, pixel_limit),
        None => GuardDecision::Block(BlockReason::ImageDimensionsUnknown),
    }
}

/// SVG バイト列から要素数を数え、暴走ガードを判定する（要件2.2）。
///
/// SVG はテキストなので要素数（`<tag` の出現数）と viewBox/属性からの推定ピクセルで暴走を防ぐ。
/// ここでは確実に取れる**要素数**を主判定にする（推定ピクセルは width/height 属性が無いことも多く、
/// 過小評価を避けるため要素数で倒す）。viewBox/width/height が取れた場合のみピクセルも併せ見る。
pub fn check_svg_bytes(bytes: &[u8], pixel_limit: u64, element_limit: u64) -> GuardDecision {
    let text = String::from_utf8_lossy(bytes);
    let element_count = count_svg_elements(&text);
    let est_pixels = svg_estimated_pixels(&text).unwrap_or(0);
    check_svg(est_pixels, element_count, pixel_limit, element_limit)
}

/// 画像ヘッダから (width, height) を読む（フルデコードしない・既知マジックのみ）。
///
/// 判定（`image_info` command）と配信前ガード（[`check_image_bytes`]）が**同一実装**で寸法を読むため
/// 公開する（U3 画像簡易ビュー・要件12.2）。判定と配信で寸法解釈が割れないよう、両者ともこの関数を
/// 唯一の寸法読取り元にする。**戻り値型 `Option<(u64, u64)>` は配信ガードの呼び出し規約のため変えない**。
pub fn image_dimensions(b: &[u8]) -> Option<(u64, u64)> {
    // PNG: 8 バイトシグネチャ + IHDR(width/height は big-endian u32)。
    if b.len() >= 24 && b.starts_with(&[0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a]) {
        let w = be_u32(&b[16..20])? as u64;
        let h = be_u32(&b[20..24])? as u64;
        return Some((w, h));
    }
    // GIF: "GIF87a"/"GIF89a" + width/height は little-endian u16。
    if b.len() >= 10 && (b.starts_with(b"GIF87a") || b.starts_with(b"GIF89a")) {
        let w = le_u16(&b[6..8])? as u64;
        let h = le_u16(&b[8..10])? as u64;
        return Some((w, h));
    }
    // BMP: "BM" + DIB ヘッダ（BITMAPINFOHEADER）の width/height は little-endian i32。
    if b.len() >= 26 && b.starts_with(b"BM") {
        let w = le_u32(&b[18..22])? as i32;
        let h = le_u32(&b[22..26])? as i32;
        return Some((w.unsigned_abs() as u64, h.unsigned_abs() as u64));
    }
    // WebP: "RIFF"...."WEBP" + VP8/VP8L/VP8X で寸法位置が異なる。
    if b.len() >= 30 && b.starts_with(b"RIFF") && &b[8..12] == b"WEBP" {
        return webp_dimensions(b);
    }
    // JPEG: SOF マーカ(SOF0..SOF15、ただし DHT/DAC/RST 等を除く)を走査して height/width を読む。
    if b.len() >= 4 && b[0] == 0xff && b[1] == 0xd8 {
        return jpeg_dimensions(b);
    }
    // ICO/CUR: reserved(2)=0 + type(2,LE)=1(ICO)/2(CUR) + count(2,LE)。
    // 全 ICONDIRENTRY を走査して最大の width*height を採る。要件の `.ico`（favicon 等）が
    // `image_dimensions` で None になり配信前ガードで誤ブロックされる回帰を防ぐ（#24 の None=>Block は維持）。
    if b.len() >= 6
        && b[0] == 0x00
        && b[1] == 0x00
        && (b[2] == 0x01 || b[2] == 0x02)
        && b[3] == 0x00
    {
        return ico_dimensions(b);
    }
    None
}

/// ICO/CUR の全 ICONDIRENTRY を走査し、最大の (width, height) を返す。
///
/// 各 ICONDIRENTRY は先頭オフセット6から 16 バイト固定:
/// +0 width(1, 0=256) / +1 height(1, 0=256) / +8 bytesInRes(4,LE) / +12 imageOffset(4,LE)。
/// エントリのデータ先頭（imageOffset）が PNG マジックなら、ディレクトリの 8bit 寸法ではなく
/// 埋め込み PNG の IHDR（PNG 先頭 +16 に width u32 BE, +20 に height u32 BE）から真の寸法を読む
/// （PNG は 256 超を表現できるため、8bit フィールドだけでは小寸法に偽装される）。
///
/// 壊れ/細工 ICO（count=0、エントリ配列やオフセット/IHDR がバッファ外、不整合）は寸法を信頼できないので
/// `None` を返す（呼び出し側 `check_image_bytes` が Block＝#24 fail-closed 方針）。配列範囲外で panic しない。
fn ico_dimensions(b: &[u8]) -> Option<(u64, u64)> {
    const PNG_MAGIC: [u8; 8] = [0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
    const DIR_HEADER: usize = 6; // ICONDIR ヘッダ長
    const ENTRY_SIZE: usize = 16; // ICONDIRENTRY 長

    let count = le_u16(b.get(4..6)?)? as usize;
    if count == 0 {
        return None; // エントリ無し＝寸法を信頼できない。
    }
    // エントリ配列がバッファ内に収まることを先に確認する（範囲外読みの防止）。
    let entries_end = DIR_HEADER.checked_add(count.checked_mul(ENTRY_SIZE)?)?;
    if entries_end > b.len() {
        return None; // count 過大でバッファ長不足。
    }

    let mut best: Option<(u64, u64)> = None;
    for i in 0..count {
        let entry = DIR_HEADER + i * ENTRY_SIZE;
        let w8 = *b.get(entry)?;
        let h8 = *b.get(entry + 1)?;
        let image_offset = le_u32(b.get(entry + 12..entry + 16)?)? as usize;

        // データ先頭が PNG マジックなら IHDR から真寸法を読む（256 超を表現できる）。
        let png_data = b.get(image_offset..);
        let (w, h) = if png_data.is_some_and(|d| d.starts_with(&PNG_MAGIC)) {
            // IHDR: PNG 先頭 +16 width u32 BE, +20 height u32 BE。範囲外/オーバーフローなら壊れ＝None。
            let w_at = image_offset.checked_add(16)?;
            let h_at = image_offset.checked_add(20)?;
            let pw = be_u32(b.get(w_at..w_at.checked_add(4)?)?)? as u64;
            let ph = be_u32(b.get(h_at..h_at.checked_add(4)?)?)? as u64;
            (pw, ph)
        } else {
            // ディレクトリの 8bit 寸法（0 は 256 を意味する ICO 仕様）。
            let w = if w8 == 0 { 256u64 } else { w8 as u64 };
            let h = if h8 == 0 { 256u64 } else { h8 as u64 };
            (w, h)
        };

        if best.is_none_or(|(bw, bh)| w.saturating_mul(h) > bw.saturating_mul(bh)) {
            best = Some((w, h));
        }
    }
    best
}

fn webp_dimensions(b: &[u8]) -> Option<(u64, u64)> {
    match &b[12..16] {
        b"VP8 " => {
            // ロッシー: 0x14 から 14 ビット width/height（+1）。
            if b.len() < 30 {
                return None;
            }
            let w = (le_u16(&b[26..28])? & 0x3fff) as u64;
            let h = (le_u16(&b[28..30])? & 0x3fff) as u64;
            Some((w, h))
        }
        b"VP8L" => {
            // ロスレス: 0x15 から 14 ビット width/height（+1）。
            if b.len() < 25 {
                return None;
            }
            let bits = u32::from(b[21])
                | (u32::from(b[22]) << 8)
                | (u32::from(b[23]) << 16)
                | (u32::from(b[24]) << 24);
            let w = (bits & 0x3fff) + 1;
            let h = ((bits >> 14) & 0x3fff) + 1;
            Some((w as u64, h as u64))
        }
        b"VP8X" => {
            // 拡張: 0x18 から 24 ビット canvas width/height（+1・little-endian）。
            if b.len() < 30 {
                return None;
            }
            let w = (u32::from(b[24]) | (u32::from(b[25]) << 8) | (u32::from(b[26]) << 16)) + 1;
            let h = (u32::from(b[27]) | (u32::from(b[28]) << 8) | (u32::from(b[29]) << 16)) + 1;
            Some((w as u64, h as u64))
        }
        _ => None,
    }
}

fn jpeg_dimensions(b: &[u8]) -> Option<(u64, u64)> {
    let mut i = 2usize;
    while i + 9 < b.len() {
        if b[i] != 0xff {
            i += 1;
            continue;
        }
        let marker = b[i + 1];
        // スタンドアロンマーカ（ペイロード長を持たない）はスキップ。
        if matches!(marker, 0xd8 | 0xd9) || (0xd0..=0xd7).contains(&marker) {
            i += 2;
            continue;
        }
        let len = be_u16(&b[i + 2..i + 4])? as usize;
        // セグメント長は長さフィールド自身の 2 バイトを含む＝最小 2（JPEG 仕様）。
        // `len < 2` は仕様違反の壊れ/細工 JPEG。前進量 `2 + len` が進まず無限ループ/誤読の温床になるため
        // 走査を**即打ち切る**（None を返す＝寸法不明）。寸法不明時の既定は配信前ガードでブロック寄りに倒す
        // （[`check_image_bytes`] の方針・#24・nontext::decide_image_open と統一）。
        if len < 2 {
            return None;
        }
        // SOF0..SOF15（0xc0..0xcf）から DHT(0xc4)/DAC(0xcc)/JPG(0xc8) を除く＝実際の SOF。
        if (0xc0..=0xcf).contains(&marker) && !matches!(marker, 0xc4 | 0xc8 | 0xcc) {
            // SOF: [len(2)][precision(1)][height(2)][width(2)]...
            if i + 9 < b.len() {
                let h = be_u16(&b[i + 5..i + 7])? as u64;
                let w = be_u16(&b[i + 7..i + 9])? as u64;
                return Some((w, h));
            }
            return None;
        }
        i += 2 + len;
    }
    None
}

/// SVG の要素数（開始タグの数）を数える。コメント/属性値内の `<` は素朴には拾わないため、
/// `<` の直後が英字（要素名開始）の出現を数える簡易計数。
///
/// 注記（#43・厳密化は不要）: **開始タグのみ計数**する（閉じタグ `</`・コメント `<!`・処理命令 `<?` は数えない）。
/// 自己終了要素（`<rect/>`）も開始タグ 1 つとして数えるため、実際の要素数を厳密に反映するわけではないが、
/// 計上は常に過小ではなく実要素数の近似（過検出側＝誤ブロックは低確率で安全側）に倒れる。要素数上限の
/// 目的は暴走の早期遮断であり、厳密な要素計数は不要なため簡易計数のままとする。
fn count_svg_elements(text: &str) -> u64 {
    let bytes = text.as_bytes();
    let mut count = 0u64;
    let mut i = 0;
    while i + 1 < bytes.len() {
        if bytes[i] == b'<' {
            let c = bytes[i + 1];
            // 開始タグ（`<tag`）のみ数える（閉じタグ `</`・コメント `<!`・処理命令 `<?` は除外）。
            if c.is_ascii_alphabetic() {
                count = count.saturating_add(1);
            }
        }
        i += 1;
    }
    count
}

/// SVG の width*height（属性 or viewBox の幅・高さ）から推定ピクセル数を出す（取れなければ None）。
fn svg_estimated_pixels(text: &str) -> Option<u64> {
    // 属性名検索用の小文字化は **1 回だけ** 行い width/height/viewBox の探索で使い回す（従来は
    // svg_attr_value 内で最大 3 回 to_ascii_lowercase していた＝軽量化）。`to_ascii_lowercase` は
    // バイト長を変えないため `lower` 上の位置は元 `text` にそのまま通用する（取得値は不変）。
    let lower = text.to_ascii_lowercase();
    let w = svg_attr_number(text, &lower, "width");
    let h = svg_attr_number(text, &lower, "height");
    if let (Some(w), Some(h)) = (w, h) {
        return Some(w.saturating_mul(h));
    }
    // viewBox="minx miny w h"
    if let Some(vb) = svg_attr_value(text, &lower, "viewbox") {
        let nums: Vec<f64> = vb
            .split([' ', ',', '\t', '\n'])
            .filter(|s| !s.is_empty())
            .filter_map(|s| s.parse::<f64>().ok())
            .collect();
        if nums.len() == 4 {
            let w = nums[2].max(0.0) as u64;
            let h = nums[3].max(0.0) as u64;
            return Some(w.saturating_mul(h));
        }
    }
    None
}

/// SVG 属性の数値（`width="600"` の 600。単位 px は許容しそれ以外は None）。
/// `lower` は `text` の小文字化済みコピー（属性名検索用・呼び出し側が 1 回生成して使い回す）。
fn svg_attr_number(text: &str, lower: &str, name: &str) -> Option<u64> {
    let raw = svg_attr_value(text, lower, name)?;
    let trimmed = raw.trim().trim_end_matches("px").trim();
    trimmed.parse::<f64>().ok().map(|v| v.max(0.0) as u64)
}

/// SVG ルートの属性値を素朴に拾う（最初の `name="..."`/`name='...'`・大小無視）。
/// `lower` は `text` を `to_ascii_lowercase` した属性名検索用文字列（呼び出し側で 1 回生成し使い回す）。
/// `to_ascii_lowercase` はバイト長を変えないため `lower` 上の位置は `text` にそのまま通用する。
fn svg_attr_value(text: &str, lower: &str, name: &str) -> Option<String> {
    let key = format!("{name}=");
    let pos = lower.find(&key)?;
    let after = &text[pos + key.len()..];
    let bytes = after.as_bytes();
    let quote = *bytes.first()?;
    if quote != b'"' && quote != b'\'' {
        return None;
    }
    let rest = &after[1..];
    let end = rest.find(quote as char)?;
    Some(rest[..end].to_string())
}

fn be_u32(b: &[u8]) -> Option<u32> {
    Some(u32::from_be_bytes(b.get(..4)?.try_into().ok()?))
}
fn be_u16(b: &[u8]) -> Option<u16> {
    Some(u16::from_be_bytes(b.get(..2)?.try_into().ok()?))
}
fn le_u16(b: &[u8]) -> Option<u16> {
    Some(u16::from_le_bytes(b.get(..2)?.try_into().ok()?))
}
fn le_u32(b: &[u8]) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(..4)?.try_into().ok()?))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 画像が6000万px以内なら許可() {
        assert!(check_image_pixels(8000, 7500, DEFAULT_IMAGE_MAX_PIXELS).is_allowed()); // 6000万px ちょうど
        assert!(check_image_pixels(1920, 1080, DEFAULT_IMAGE_MAX_PIXELS).is_allowed());
    }

    #[test]
    fn 画像が6000万pxを超えるとブロック() {
        let d = check_image_pixels(10_000, 7_000, DEFAULT_IMAGE_MAX_PIXELS); // 7000万px
        assert_eq!(
            d,
            GuardDecision::Block(BlockReason::ImagePixels {
                pixels: 70_000_000,
                limit: DEFAULT_IMAGE_MAX_PIXELS
            })
        );
    }

    #[test]
    fn 画像の寸法乗算がオーバーフローしない() {
        // saturating で u64 を溢れさせない（巨大寸法の悪意ある画像）。
        let d = check_image_pixels(u64::MAX, 2, DEFAULT_IMAGE_MAX_PIXELS);
        assert!(!d.is_allowed());
    }

    #[test]
    fn svg_は推定px超過でブロック() {
        let d = check_svg(
            90_000_000,
            100,
            DEFAULT_SVG_MAX_PIXELS,
            DEFAULT_SVG_MAX_ELEMENTS,
        );
        assert_eq!(
            d,
            GuardDecision::Block(BlockReason::SvgPixels {
                pixels: 90_000_000,
                limit: DEFAULT_SVG_MAX_PIXELS
            })
        );
    }

    #[test]
    fn svg_は要素数超過でブロック() {
        let d = check_svg(
            1000,
            60_000,
            DEFAULT_SVG_MAX_PIXELS,
            DEFAULT_SVG_MAX_ELEMENTS,
        );
        assert_eq!(
            d,
            GuardDecision::Block(BlockReason::SvgElements {
                elements: 60_000,
                limit: DEFAULT_SVG_MAX_ELEMENTS
            })
        );
    }

    #[test]
    fn svg_は上限内なら許可() {
        assert!(check_svg(
            1_000_000,
            1000,
            DEFAULT_SVG_MAX_PIXELS,
            DEFAULT_SVG_MAX_ELEMENTS
        )
        .is_allowed());
    }

    #[test]
    fn 長行ガード() {
        let long = "a".repeat(DEFAULT_LONG_LINE_CHARS + 1);
        assert!(has_long_line(&long, DEFAULT_LONG_LINE_CHARS));
        let short = "a".repeat(DEFAULT_LONG_LINE_CHARS);
        assert!(!has_long_line(&short, DEFAULT_LONG_LINE_CHARS));
        assert!(!has_long_line(
            "short\nlines\nhere",
            DEFAULT_LONG_LINE_CHARS
        ));
    }

    #[test]
    fn html_タイムアウト既定値() {
        assert_eq!(DEFAULT_HTML_TIMEOUT_MS, 10_000);
    }

    /// PNG ヘッダ（シグネチャ + IHDR の width/height）を組み立てる。
    fn png_header(w: u32, h: u32) -> Vec<u8> {
        let mut b = vec![0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
        b.extend_from_slice(&[0, 0, 0, 13]); // IHDR 長
        b.extend_from_slice(b"IHDR");
        b.extend_from_slice(&w.to_be_bytes());
        b.extend_from_slice(&h.to_be_bytes());
        b.extend_from_slice(&[8, 2, 0, 0, 0]); // bit depth 等（残り）
        b
    }

    #[test]
    fn png_ヘッダから寸法を読み巨大画像をブロックする() {
        // 1920x1080 は許可。
        assert!(check_image_bytes(&png_header(1920, 1080), DEFAULT_IMAGE_MAX_PIXELS).is_allowed());
        // 10000x7000 = 7000万px は 6000万px 超でブロック（デコードしない）。
        let d = check_image_bytes(&png_header(10_000, 7_000), DEFAULT_IMAGE_MAX_PIXELS);
        assert_eq!(
            d,
            GuardDecision::Block(BlockReason::ImagePixels {
                pixels: 70_000_000,
                limit: DEFAULT_IMAGE_MAX_PIXELS
            })
        );
    }

    #[test]
    fn gif_ヘッダから寸法を読む() {
        let mut b = b"GIF89a".to_vec();
        b.extend_from_slice(&10000u16.to_le_bytes());
        b.extend_from_slice(&7000u16.to_le_bytes());
        // 10000*7000=7000万px > 6000万px。
        assert!(!check_image_bytes(&b, DEFAULT_IMAGE_MAX_PIXELS).is_allowed());
    }

    #[test]
    fn jpeg_の_sof_マーカから寸法を読む() {
        // SOI + APP0(JFIF・長 16) + SOF0(長 17・height/width)。
        let mut b = vec![0xff, 0xd8];
        // APP0
        b.extend_from_slice(&[0xff, 0xe0, 0x00, 0x10]);
        b.extend_from_slice(b"JFIF\0");
        b.extend_from_slice(&[0u8; 9]);
        // SOF0
        b.extend_from_slice(&[0xff, 0xc0, 0x00, 0x11, 0x08]);
        b.extend_from_slice(&7000u16.to_be_bytes()); // height
        b.extend_from_slice(&10000u16.to_be_bytes()); // width
        b.extend_from_slice(&[0u8; 8]);
        let d = check_image_bytes(&b, DEFAULT_IMAGE_MAX_PIXELS);
        assert!(!d.is_allowed(), "JPEG 7000万px がブロックされない: {d:?}");
    }

    /// ICO ヘッダ（ICONDIR + 先頭 ICONDIRENTRY の width/height）を組み立てる。
    /// `w8`/`h8` は 8bit フィールド値（0 は 256 を意味する ICO 仕様）。
    fn ico_header(w8: u8, h8: u8) -> Vec<u8> {
        let mut b = vec![0x00, 0x00, 0x01, 0x00]; // reserved=0, type=1(ICO)
        b.extend_from_slice(&1u16.to_le_bytes()); // count=1
        b.push(w8); // ICONDIRENTRY.width（オフセット6）
        b.push(h8); // ICONDIRENTRY.height（オフセット7）
                    // ICONDIRENTRY 残り 14 バイト（colorCount/reserved/planes/bitCount/bytesInRes/imageOffset）。
                    // imageOffset=0 とし PNG マジックには一致させない（ディレクトリ 8bit 寸法を使わせる）。
        b.extend_from_slice(&[0u8; 14]);
        b
    }

    #[test]
    fn ico_ヘッダから寸法を読み小寸法は許可する() {
        // #24 の回帰修正: ICO（favicon 等）が image_dimensions で None になり配信前ガードで
        // 誤ブロックされていた。16x16 の ICO は寸法を読めて Allow になること。
        let b = ico_header(16, 16);
        assert_eq!(image_dimensions(&b), Some((16, 16)));
        let d = check_image_bytes(&b, DEFAULT_IMAGE_MAX_PIXELS);
        assert!(d.is_allowed(), "小寸法 ICO がブロックされた: {d:?}");
    }

    #[test]
    fn ico_の0サイズフィールドは256として扱う() {
        // ICO 仕様: width/height の 8bit フィールドが 0 のときは 256。
        let b = ico_header(0, 0);
        assert_eq!(image_dimensions(&b), Some((256, 256)));
        assert!(check_image_bytes(&b, DEFAULT_IMAGE_MAX_PIXELS).is_allowed());
    }

    /// 任意エントリ数の ICO を組み立てる。各タプルは (8bit width, 8bit height, imageOffset, imageData)。
    /// imageData は imageOffset 位置に配置する（PNG 埋め込みなどの細工に使う）。
    fn ico_multi(entries: &[(u8, u8, u32, Vec<u8>)]) -> Vec<u8> {
        let count = entries.len() as u16;
        let mut b = vec![0x00, 0x00, 0x01, 0x00];
        b.extend_from_slice(&count.to_le_bytes());
        for (w8, h8, off, _) in entries {
            b.push(*w8);
            b.push(*h8);
            b.extend_from_slice(&[0u8; 6]); // colorCount/reserved/planes/bitCount
            b.extend_from_slice(&0u32.to_le_bytes()); // bytesInRes（寸法判定には未使用）
            b.extend_from_slice(&off.to_le_bytes()); // imageOffset
        }
        for (_, _, off, data) in entries {
            let off = *off as usize;
            if off >= b.len() && !data.is_empty() {
                b.resize(off, 0);
                b.extend_from_slice(data);
            }
        }
        b
    }

    /// 埋め込み用 PNG（IHDR の width/height のみ妥当・残りはダミー）を作る。
    fn png_with_dims(w: u32, h: u32) -> Vec<u8> {
        let mut p = vec![0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
        p.extend_from_slice(&0u32.to_be_bytes()); // IHDR 長（未使用）
        p.extend_from_slice(b"IHDR");
        p.extend_from_slice(&w.to_be_bytes()); // +16 width BE
        p.extend_from_slice(&h.to_be_bytes()); // +20 height BE
        p
    }

    #[test]
    fn ico_多重エントリは最大寸法を採る() {
        // 先頭は小寸法（16x16）、後続にディレクトリ寸法 256x256 を持たせる。最大が採られること。
        let b = ico_multi(&[(16, 16, 0, vec![]), (0, 0, 0, vec![])]); // 0=>256
        assert_eq!(image_dimensions(&b), Some((256, 256)));
    }

    #[test]
    fn ico_後続のpng埋め込み巨大画像はブロックする() {
        // 先頭は小寸法だが、後続エントリのデータが 20000x20000 の PNG。真寸法を採って Block。
        let png = png_with_dims(20000, 20000);
        let b = ico_multi(&[(16, 16, 0, vec![]), (16, 16, 100, png)]);
        let d = image_dimensions(&b);
        assert_eq!(
            d,
            Some((20000, 20000)),
            "PNG IHDR の真寸法が採られない: {d:?}"
        );
        let dec = check_image_bytes(&b, DEFAULT_IMAGE_MAX_PIXELS);
        assert!(
            !dec.is_allowed(),
            "巨大 PNG 埋め込み ICO がブロックされない: {dec:?}"
        );
    }

    #[test]
    fn ico_壊れ入力はnoneでブロックしパニックしない() {
        // count 過大でエントリ配列がバッファ外。
        let mut over = vec![0x00, 0x00, 0x01, 0x00];
        over.extend_from_slice(&9999u16.to_le_bytes()); // count=9999 だが本体なし
        assert_eq!(image_dimensions(&over), None);

        // imageOffset がバッファ外（PNG マジック判定もできない）→ 8bit 寸法にフォールバック（壊れではない）。
        // ここでは imageOffset が PNG マジックを指すが IHDR が欠落しているケースを検証する。
        let mut trunc = png_with_dims(20000, 20000);
        trunc.truncate(8 + 4 + 4 + 2); // IHDR の width 途中で切る
        let b = ico_multi(&[(16, 16, 100, trunc)]);
        assert_eq!(image_dimensions(&b), None, "IHDR 欠落で None にならない");

        // count=0。
        let mut zero = vec![0x00, 0x00, 0x01, 0x00];
        zero.extend_from_slice(&0u16.to_le_bytes());
        assert_eq!(image_dimensions(&zero), None);

        // 大量データで panic しないこと（範囲外アクセス回帰）。
        let mut huge_off = vec![0x00, 0x00, 0x01, 0x00];
        huge_off.extend_from_slice(&1u16.to_le_bytes());
        huge_off.push(16);
        huge_off.push(16);
        huge_off.extend_from_slice(&[0u8; 6]);
        huge_off.extend_from_slice(&0u32.to_le_bytes());
        huge_off.extend_from_slice(&u32::MAX.to_le_bytes()); // imageOffset=u32::MAX
                                                             // PNG マジックには一致しないので 8bit 寸法 16x16 を採る（panic しないことが主眼）。
        assert_eq!(image_dimensions(&huge_off), Some((16, 16)));
    }

    #[test]
    fn 寸法不明の画像はfail_closedでブロックする() {
        // #24: ヘッダから寸法を取れない（マジック不一致/壊れ/細工）画像は配信前ガードでブロックし
        // 「既定アプリで開く」へ誘導する（nontext::decide_image_open の寸法不明時方針と統一）。
        let d = check_image_bytes(b"not an image", DEFAULT_IMAGE_MAX_PIXELS);
        assert_eq!(d, GuardDecision::Block(BlockReason::ImageDimensionsUnknown));
    }

    #[test]
    fn jpeg_の不正なセグメント長は走査を打ち切りブロックする() {
        // #24: len < 2 は JPEG 仕様違反（長さフィールド 2 バイトを含むため最小 2）。
        // SOF を見つけられず寸法不明＝fail-closed でブロックする（無限ループ/誤読を防ぐ）。
        let mut b = vec![0xff, 0xd8]; // SOI
        b.extend_from_slice(&[0xff, 0xe0, 0x00, 0x00]); // APP0 だが len=0（不正・< 2）
        b.extend_from_slice(&[0u8; 16]);
        let d = check_image_bytes(&b, DEFAULT_IMAGE_MAX_PIXELS);
        assert_eq!(
            d,
            GuardDecision::Block(BlockReason::ImageDimensionsUnknown),
            "不正セグメント長 JPEG が寸法不明（ブロック）にならない: {d:?}"
        );
    }

    #[test]
    fn svg_バイト列の要素数超過をブロックする() {
        // 5万要素超の SVG（要素数で倒す）。
        let mut svg = String::from("<svg xmlns=\"http://www.w3.org/2000/svg\">");
        for _ in 0..60_000 {
            svg.push_str("<rect/>");
        }
        svg.push_str("</svg>");
        let d = check_svg_bytes(
            svg.as_bytes(),
            DEFAULT_SVG_MAX_PIXELS,
            DEFAULT_SVG_MAX_ELEMENTS,
        );
        assert!(
            matches!(d, GuardDecision::Block(BlockReason::SvgElements { .. })),
            "{d:?}"
        );
    }

    #[test]
    fn svg_バイト列の推定px超過をブロックする() {
        let svg = r#"<svg width="20000" height="5000"><rect/></svg>"#; // 1億px > 8000万px
        let d = check_svg_bytes(
            svg.as_bytes(),
            DEFAULT_SVG_MAX_PIXELS,
            DEFAULT_SVG_MAX_ELEMENTS,
        );
        assert!(
            matches!(d, GuardDecision::Block(BlockReason::SvgPixels { .. })),
            "{d:?}"
        );
    }

    #[test]
    fn svg_バイト列の_viewbox_から推定pxを取る() {
        let svg = r#"<svg viewBox="0 0 20000 5000"><rect/></svg>"#; // 1億px
        let d = check_svg_bytes(
            svg.as_bytes(),
            DEFAULT_SVG_MAX_PIXELS,
            DEFAULT_SVG_MAX_ELEMENTS,
        );
        assert!(
            matches!(d, GuardDecision::Block(BlockReason::SvgPixels { .. })),
            "{d:?}"
        );
    }

    #[test]
    fn 通常の_svg_は許可する() {
        let svg =
            r#"<svg width="600" height="400"><rect x="0" y="0" width="100" height="100"/></svg>"#;
        assert!(check_svg_bytes(
            svg.as_bytes(),
            DEFAULT_SVG_MAX_PIXELS,
            DEFAULT_SVG_MAX_ELEMENTS
        )
        .is_allowed());
    }

    /// BMP ヘッダ（"BM" + DIB の width/height は little-endian i32・offset 18/22）を組み立てる。
    fn bmp_header(w: i32, h: i32) -> Vec<u8> {
        let mut b = b"BM".to_vec();
        b.extend_from_slice(&[0u8; 16]); // offset 2..18（ファイルヘッダ残り + DIB ヘッダ先頭）
        b.extend_from_slice(&w.to_le_bytes()); // offset 18..22 width(i32 LE)
        b.extend_from_slice(&h.to_le_bytes()); // offset 22..26 height(i32 LE)
        b
    }

    #[test]
    fn bmp_ヘッダから寸法を読み小寸法は許可し巨大はブロックする() {
        // 小寸法（1920x1080）は寸法を読めて Allow。
        assert_eq!(
            image_dimensions(&bmp_header(1920, 1080)),
            Some((1920, 1080))
        );
        assert!(check_image_bytes(&bmp_header(1920, 1080), DEFAULT_IMAGE_MAX_PIXELS).is_allowed());
        // 10000x7000 = 7000万px は 6000万px 超でブロック（デコードしない）。
        let d = check_image_bytes(&bmp_header(10_000, 7_000), DEFAULT_IMAGE_MAX_PIXELS);
        assert_eq!(
            d,
            GuardDecision::Block(BlockReason::ImagePixels {
                pixels: 70_000_000,
                limit: DEFAULT_IMAGE_MAX_PIXELS
            })
        );
    }

    #[test]
    fn bmp_の負の高さ_top_down_は絶対値で寸法を読む() {
        // BMP は height 負値で top-down 格納。実装は unsigned_abs で寸法を取る。
        assert_eq!(image_dimensions(&bmp_header(640, -480)), Some((640, 480)));
    }

    /// WebP(VP8・ロッシー)ヘッダ。width=b[26..28]&0x3fff / height=b[28..30]&0x3fff（実装どおり）。
    fn webp_vp8_header(w: u16, h: u16) -> Vec<u8> {
        let mut b = b"RIFF".to_vec();
        b.extend_from_slice(&[0u8; 4]); // file size（寸法判定に未使用）
        b.extend_from_slice(b"WEBP");
        b.extend_from_slice(b"VP8 ");
        b.extend_from_slice(&[0u8; 10]); // 16..26（チャンク長/フレームタグ/開始コード等・未使用）
        b.extend_from_slice(&w.to_le_bytes()); // 26..28 width
        b.extend_from_slice(&h.to_le_bytes()); // 28..30 height
        b
    }

    /// WebP(VP8L・ロスレス)ヘッダ。b[21..25] の 32bit に (width-1)|((height-1)<<14) を詰める。
    fn webp_vp8l_header(w: u32, h: u32) -> Vec<u8> {
        let mut b = b"RIFF".to_vec();
        b.extend_from_slice(&[0u8; 4]);
        b.extend_from_slice(b"WEBP");
        b.extend_from_slice(b"VP8L");
        b.extend_from_slice(&[0u8; 4]); // 16..20 チャンク長（未使用）
        b.push(0x2f); // 20: VP8L シグネチャ
        let bits = ((w - 1) & 0x3fff) | (((h - 1) & 0x3fff) << 14);
        b.extend_from_slice(&bits.to_le_bytes()); // 21..25
        b.resize(30, 0); // 外側ガード（b.len() >= 30）を満たすよう padding
        b
    }

    /// WebP(VP8X・拡張)ヘッダ。width-1=b[24..27] / height-1=b[27..30] の 24bit LE。
    fn webp_vp8x_header(w: u32, h: u32) -> Vec<u8> {
        let mut b = b"RIFF".to_vec();
        b.extend_from_slice(&[0u8; 4]);
        b.extend_from_slice(b"WEBP");
        b.extend_from_slice(b"VP8X");
        b.extend_from_slice(&[0u8; 8]); // 16..24 チャンク長(4)+フラグ(4)（未使用）
        let wm1 = w - 1;
        let hm1 = h - 1;
        for v in [wm1, hm1] {
            b.push((v & 0xff) as u8);
            b.push(((v >> 8) & 0xff) as u8);
            b.push(((v >> 16) & 0xff) as u8);
        }
        b
    }

    #[test]
    fn webp_vp8_ヘッダから寸法を読み小寸法は許可し巨大はブロックする() {
        assert_eq!(
            image_dimensions(&webp_vp8_header(1920, 1080)),
            Some((1920, 1080))
        );
        assert!(
            check_image_bytes(&webp_vp8_header(1920, 1080), DEFAULT_IMAGE_MAX_PIXELS).is_allowed()
        );
        let d = check_image_bytes(&webp_vp8_header(10_000, 7_000), DEFAULT_IMAGE_MAX_PIXELS);
        assert_eq!(
            d,
            GuardDecision::Block(BlockReason::ImagePixels {
                pixels: 70_000_000,
                limit: DEFAULT_IMAGE_MAX_PIXELS
            })
        );
    }

    #[test]
    fn webp_vp8l_ヘッダから寸法を読み小寸法は許可し巨大はブロックする() {
        assert_eq!(
            image_dimensions(&webp_vp8l_header(1920, 1080)),
            Some((1920, 1080))
        );
        assert!(
            check_image_bytes(&webp_vp8l_header(1920, 1080), DEFAULT_IMAGE_MAX_PIXELS).is_allowed()
        );
        let d = check_image_bytes(&webp_vp8l_header(10_000, 7_000), DEFAULT_IMAGE_MAX_PIXELS);
        assert_eq!(
            d,
            GuardDecision::Block(BlockReason::ImagePixels {
                pixels: 70_000_000,
                limit: DEFAULT_IMAGE_MAX_PIXELS
            })
        );
    }

    #[test]
    fn webp_vp8x_ヘッダから寸法を読み小寸法は許可し巨大はブロックする() {
        assert_eq!(
            image_dimensions(&webp_vp8x_header(1920, 1080)),
            Some((1920, 1080))
        );
        assert!(
            check_image_bytes(&webp_vp8x_header(1920, 1080), DEFAULT_IMAGE_MAX_PIXELS).is_allowed()
        );
        let d = check_image_bytes(&webp_vp8x_header(10_000, 7_000), DEFAULT_IMAGE_MAX_PIXELS);
        assert_eq!(
            d,
            GuardDecision::Block(BlockReason::ImagePixels {
                pixels: 70_000_000,
                limit: DEFAULT_IMAGE_MAX_PIXELS
            })
        );
    }
}
