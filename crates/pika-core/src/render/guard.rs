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
        assert!(!has_long_line("short\nlines\nhere", DEFAULT_LONG_LINE_CHARS));
    }

    #[test]
    fn html_タイムアウト既定値() {
        assert_eq!(DEFAULT_HTML_TIMEOUT_MS, 10_000);
    }
}
