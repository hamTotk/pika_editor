//! プレビューレンダリング（全Web化の最重要セキュリティ境界・要件6・design doc 6章/7章）。
//!
//! 本モジュールは UI/Tauri/wry/WebView を一切知らない純粋ロジック（cargo test の決定論ゲート対象）。
//! 未信頼文書（AI 出力の Markdown/HTML）を**権限ゼロの別WebView**へ custom protocol で直配信する際の
//! 「サニタイズ済み HTML」と「CSP レスポンスヘッダ」と「nonce」を組み立てる。多層防御の各層:
//!
//! 1. [`sanitize`] — comrak(`unsafe_`)→ammonia 最終段サニタイズ（script/on*/javascript:/iframe/meta 除去・
//!    DOM clobbering 防止・SVG サブセット）。系統A（Markdown/差分/SVG・信頼JS）/系統B（HTML・JS無効）。
//! 2. [`csp`] — レスポンスヘッダで強制する CSP（既定外部遮断・nonce・オプトイン緩和は img/font のみ）。
//! 3. [`path`] — ローカル相対参照の封じ込め（canonicalize+prefix・絶対/`..`/シンボリックリンク脱出/機密拒否）。
//! 4. [`guard`] — 暴走ガード（画像6000万px・SVG8000万px/5万要素・HTML10秒・長行）を入力段で計測。
//!
//! 「別WebView の capability ゼロ」（Tauri API 到達不能）は src-tauri 側の配線で担保する（design doc 6章）。

pub mod csp;
pub mod guard;
pub mod path;
pub mod sanitize;

pub use csp::{build_csp, generate_nonce, ExternalResourceAllow, Nonce};
pub use guard::{
    check_image_pixels, check_svg, has_long_line, BlockReason, GuardDecision,
    DEFAULT_HTML_TIMEOUT_MS, DEFAULT_IMAGE_MAX_PIXELS, DEFAULT_LONG_LINE_CHARS,
    DEFAULT_SVG_MAX_ELEMENTS, DEFAULT_SVG_MAX_PIXELS,
};
pub use path::{confine_under, join_under, resolve_local_ref, LocalRefDecision, RejectReason};
pub use sanitize::{markdown_to_unsafe_html, sanitize_html, PreviewFlavor};

/// custom protocol が別WebView へ直配信する 1 レスポンス分（要件6・design doc 6章）。
///
/// `body`（サニタイズ済み HTML）と `csp`（レスポンスヘッダ値）を**必ずセットで**配信する。
/// frontend は別WebView の URL を切り替えるだけで、`body` は JS のメインワールドを一切経由しない。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PreviewResponse {
    /// サニタイズ済み HTML 本体（[`sanitize_html`] を通過済み＝呼び出し規約）。
    pub body: String,
    /// `Content-Security-Policy` レスポンスヘッダ値（[`build_csp`]）。
    pub csp: String,
    /// 系統A の `script-src 'nonce-<rnd>'`。信頼 JS 注入に同じ値を使う。系統B では空。
    pub nonce: Nonce,
    /// 系統（A/B）。
    pub flavor: PreviewFlavor,
}

/// Markdown 文書をプレビュー用に変換する（系統A・要件6.2）。
///
/// comrak(`unsafe_`) で HTML 化 → ammonia 最終段サニタイズ → CSP（系統A・nonce）を組む。
/// `allow` はオプトイン外部許可（既定 [`ExternalResourceAllow::blocked`]＝外部遮断）。
pub fn prepare_markdown_preview(markdown: &str, allow: &ExternalResourceAllow) -> PreviewResponse {
    let unsafe_html = markdown_to_unsafe_html(markdown);
    let body = sanitize_html(&unsafe_html, PreviewFlavor::MarkdownTrustedJs);
    let nonce = generate_nonce();
    let csp = build_csp(PreviewFlavor::MarkdownTrustedJs, &nonce, allow);
    PreviewResponse {
        body,
        csp,
        nonce,
        flavor: PreviewFlavor::MarkdownTrustedJs,
    }
}

/// ユーザー文書の HTML をプレビュー用にサニタイズする（系統B・要件6.3）。
///
/// 文書 JS は完全無効（CSP `script-src 'none'`・ammonia でも script 除去）。インライン CSS は尊重する。
/// `<meta refresh>` は ammonia 段で除去する（JS 無効でも自動遷移するため）。
pub fn prepare_html_preview(html: &str, allow: &ExternalResourceAllow) -> PreviewResponse {
    let body = sanitize_html(html, PreviewFlavor::HtmlNoJs);
    // 系統B は nonce を使わないが、CSP 組立 API の引数に空文字を渡す（script-src 'none' で無視される）。
    let nonce = String::new();
    let csp = build_csp(PreviewFlavor::HtmlNoJs, &nonce, allow);
    PreviewResponse {
        body,
        csp,
        nonce,
        flavor: PreviewFlavor::HtmlNoJs,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn markdown_プレビューは系統aでscriptを除去しnonce_cspを返す() {
        let resp = prepare_markdown_preview(
            "# タイトル\n\n<script>alert(1)</script>\n\n本文",
            &ExternalResourceAllow::blocked(),
        );
        assert_eq!(resp.flavor, PreviewFlavor::MarkdownTrustedJs);
        assert!(resp.body.contains("<h1"), "見出しが消えた: {}", resp.body);
        assert!(!resp.body.contains("<script"), "script が残った: {}", resp.body);
        assert!(
            resp.csp.contains(&format!("script-src 'nonce-{}'", resp.nonce)),
            "CSP に nonce が無い: {}",
            resp.csp
        );
        assert!(!resp.nonce.is_empty());
    }

    #[test]
    fn html_プレビューは系統bでscriptを無効化しインラインcssを残す() {
        let resp = prepare_html_preview(
            r#"<div style="color:red">x</div><script>evil()</script>"#,
            &ExternalResourceAllow::blocked(),
        );
        assert_eq!(resp.flavor, PreviewFlavor::HtmlNoJs);
        assert!(resp.body.contains("style="), "インライン CSS が消えた: {}", resp.body);
        assert!(!resp.body.contains("<script"), "script が残った: {}", resp.body);
        assert!(resp.csp.contains("script-src 'none'"), "CSP: {}", resp.csp);
        assert!(resp.nonce.is_empty(), "系統B に nonce が付いた");
    }

    #[test]
    fn 既定で外部遮断_緩和なし() {
        let resp = prepare_markdown_preview("![x](http://evil/a.png)", &ExternalResourceAllow::blocked());
        assert!(resp.csp.contains("img-src pika-preview:;"), "{}", resp.csp);
        assert!(resp.csp.contains("connect-src 'none'"), "{}", resp.csp);
    }
}
