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

/// 画像の総ピクセル数の既定上限（要件2.2: 6000万px）。これを**超える**とデコードせず誘導。
pub const DEFAULT_IMAGE_MAX_PIXELS: u64 = 60_000_000;
/// SVG の展開ピクセル数相当の既定上限（要件2.2: 8000万px）。
pub const DEFAULT_SVG_MAX_PIXELS: u64 = 80_000_000;
/// SVG の要素数の既定上限（要件2.2: 5万要素）。
pub const DEFAULT_SVG_MAX_ELEMENTS: u64 = 50_000;
/// HTML レンダリングのタイムアウト（要件2.2: 10 秒）。描画側へ供給する値。
pub const DEFAULT_HTML_TIMEOUT_MS: u64 = 10_000;
/// 1 行の長さガード（要件2.2: 10万字超でハイライト/折返し自動オフ）。
pub const DEFAULT_LONG_LINE_CHARS: usize = 100_000;

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
/// 対応フォーマット（ヘッダ寸法を取れるもの）: PNG / GIF / JPEG / BMP / WebP(VP8/VP8L/VP8X)。
/// 寸法を判定できない形式（SVG は [`check_svg_bytes`] 側）は安全側で [`GuardDecision::Allow`]
/// （寸法不明＝ラスタでない/小さい想定。誤ブロックよりは通す。実消費は WebView 側で頭打ち）。
pub fn check_image_bytes(bytes: &[u8], pixel_limit: u64) -> GuardDecision {
    match image_dimensions(bytes) {
        Some((w, h)) => check_image_pixels(w, h, pixel_limit),
        None => GuardDecision::Allow,
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
fn image_dimensions(b: &[u8]) -> Option<(u64, u64)> {
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
    None
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

/// SVG の要素数（開きタグの数）を数える。コメント/属性値内の `<` は素朴には拾わないため、
/// `<` の直後が英字（要素名開始）または `/`（閉じタグ）を起点に数える簡易計数。
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
    let w = svg_attr_number(text, "width");
    let h = svg_attr_number(text, "height");
    if let (Some(w), Some(h)) = (w, h) {
        return Some((w as u64).saturating_mul(h as u64));
    }
    // viewBox="minx miny w h"
    if let Some(vb) = svg_attr_value(text, "viewbox") {
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
fn svg_attr_number(text: &str, name: &str) -> Option<u64> {
    let raw = svg_attr_value(text, name)?;
    let trimmed = raw.trim().trim_end_matches("px").trim();
    trimmed.parse::<f64>().ok().map(|v| v.max(0.0) as u64)
}

/// SVG ルートの属性値を素朴に拾う（最初の `name="..."`/`name='...'`・大小無視）。
fn svg_attr_value(text: &str, name: &str) -> Option<String> {
    let lower = text.to_ascii_lowercase();
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

    #[test]
    fn 寸法不明の画像は安全側で許可する() {
        // マジック不一致（テキスト等）は寸法不明＝Allow（誤ブロックしない）。
        assert!(check_image_bytes(b"not an image", DEFAULT_IMAGE_MAX_PIXELS).is_allowed());
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
}
