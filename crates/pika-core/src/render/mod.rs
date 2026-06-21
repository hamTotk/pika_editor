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

pub use csp::{build_csp, generate_nonce, validate_allow_hosts, ExternalResourceAllow, Nonce};
pub use guard::{
    check_image_bytes, check_image_pixels, check_svg, check_svg_bytes, has_long_line, BlockReason,
    GuardDecision, DEFAULT_HTML_TIMEOUT_MS, DEFAULT_IMAGE_MAX_PIXELS, DEFAULT_LONG_LINE_CHARS,
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
    // CSP インジェクション防止: 許可ホストに不正要素があれば全外部許可を破棄して既定（遮断）へ倒す。
    let safe_allow = sanitize_allow(allow);
    let csp = build_csp(PreviewFlavor::MarkdownTrustedJs, &nonce, &safe_allow);
    PreviewResponse {
        body,
        csp,
        nonce,
        flavor: PreviewFlavor::MarkdownTrustedJs,
    }
}

/// サニタイズ済み body を**最小の完全 HTML 文書**にラップする（別WebView へ直配信する素体）。
///
/// 役割（design doc 6章/3章「薄い境界」）: custom protocol が配信するのは `<!DOCTYPE>`/`<head>` を含む
/// 完全文書である必要がある（フラグメントのままだと charset 未指定で日本語が化け、base CSS も当たらない）。
/// 本関数は **純粋な String 操作**に徹し、Tauri を一切知らない（cargo test の決定論ゲート対象）。
///
/// セキュリティ上の不変条件（Stage ① で死守）:
/// - **`body` を改変しない**（[`sanitize_html`] 通過済みの本体をそのまま `<body>` 内へ埋め込む）。
/// - **`<meta http-equiv>` CSP を入れない**（CSP はレスポンスヘッダで強制する＝[`csp`]）。`<meta charset>` のみ。
/// - **このStageではスクリプト/ベンダーアセットを注入しない**（Mermaid/KaTeX/highlight は後続Stage）。
///   `nonce`/`flavor` は将来の信頼 JS 注入位置を確定するため引数に取るが、本実装では使用しない。
///
/// 最小 base CSS（`<style>`）は読みやすさのためのみで、CSP の `style-src 'unsafe-inline'`（系統A/B とも許可）
/// の範囲に収まる。文書由来 CSS（系統B のインライン CSS）は `body` 側に既に含まれ、ここでは上書きしない。
pub fn wrap_preview_document(body: &str, _nonce: &str, _flavor: PreviewFlavor) -> String {
    // 最小 base CSS: 余白・行間・等幅・画像はみ出し抑制のみ（質感/テーマは後続Stageで CSS 変数受け渡し）。
    const BASE_CSS: &str = "\
html{color-scheme:light dark}\
body{margin:0;padding:16px;font-family:\"Segoe UI\",\"Meiryo\",\"Yu Gothic UI\",sans-serif;\
line-height:1.6;word-wrap:break-word;overflow-wrap:anywhere}\
img{max-width:100%;height:auto}\
pre{overflow:auto}\
table{border-collapse:collapse}\
th,td{border:1px solid currentColor;padding:.3em .6em}";
    format!(
        "<!DOCTYPE html>\n<html lang=\"ja\">\n<head>\n<meta charset=\"utf-8\">\n\
<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n\
<style>{BASE_CSS}</style>\n</head>\n<body>\n{body}\n</body>\n</html>\n"
    )
}

/// オプトイン外部許可を CSP へ渡す前に検証し、不正があれば**外部遮断（既定）へ倒す**（fail-closed）。
///
/// [`validate_allow_hosts`] で 1 つでも不正ホストが見つかれば許可リスト全体を破棄する
/// （部分採用で攻撃者制御文字列が CSP へ漏れるのを避ける＝要件6.2/6.3 の「既定は必ずオフに戻る」と整合）。
fn sanitize_allow(allow: &ExternalResourceAllow) -> ExternalResourceAllow {
    if validate_allow_hosts(allow).is_ok() {
        allow.clone()
    } else {
        ExternalResourceAllow::blocked()
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
    // CSP インジェクション防止: 系統B も同様に fail-closed で外部許可を検証する。
    let safe_allow = sanitize_allow(allow);
    let csp = build_csp(PreviewFlavor::HtmlNoJs, &nonce, &safe_allow);
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
        assert!(
            !resp.body.contains("<script"),
            "script が残った: {}",
            resp.body
        );
        assert!(
            resp.csp
                .contains(&format!("script-src 'nonce-{}'", resp.nonce)),
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
        assert!(
            resp.body.contains("style="),
            "インライン CSS が消えた: {}",
            resp.body
        );
        assert!(
            !resp.body.contains("<script"),
            "script が残った: {}",
            resp.body
        );
        assert!(resp.csp.contains("script-src 'none'"), "CSP: {}", resp.csp);
        assert!(resp.nonce.is_empty(), "系統B に nonce が付いた");
    }

    #[test]
    fn 既定で外部遮断_緩和なし() {
        let resp =
            prepare_markdown_preview("![x](http://evil/a.png)", &ExternalResourceAllow::blocked());
        assert!(resp.csp.contains("img-src pika-preview:;"), "{}", resp.csp);
        assert!(resp.csp.contains("connect-src 'none'"), "{}", resp.csp);
    }

    #[test]
    fn 不正な外部許可ホストは_csp_へ漏れず外部遮断へ倒れる() {
        // CSP インジェクション余地（high 指摘）: prepare 経路で fail-closed になることを担保する。
        let allow = ExternalResourceAllow {
            hosts: vec!["https://evil.com; script-src *".to_string()],
        };
        let resp = prepare_markdown_preview("![x](rel.png)", &allow);
        assert!(
            !resp.csp.contains("script-src *"),
            "不正ホストが CSP に漏れた: {}",
            resp.csp
        );
        assert!(
            resp.csp
                .contains(&format!("script-src 'nonce-{}'", resp.nonce)),
            "nonce 限定が破られた: {}",
            resp.csp
        );
        // 1 つでも不正があれば外部遮断（既定）へ倒れる。
        assert!(
            resp.csp.contains("img-src pika-preview:;"),
            "外部遮断に戻っていない: {}",
            resp.csp
        );
    }

    #[test]
    fn wrap_は完全文書にし_charset_を入れ_body_を改変しない() {
        // Stage ①: フラグメント body を完全 HTML 文書へラップする。
        // 不変条件: DOCTYPE/charset 付与・body を一字一句改変しない・meta CSP を入れない。
        let body = "<h1>見出し</h1>\n<p>日本語の本文 &amp; テスト</p>";
        let doc = wrap_preview_document(body, "abc123", PreviewFlavor::MarkdownTrustedJs);
        assert!(doc.starts_with("<!DOCTYPE html>"), "DOCTYPE が無い: {doc}");
        assert!(
            doc.contains("<meta charset=\"utf-8\">"),
            "charset が無い（日本語化けの危険）: {doc}"
        );
        assert!(doc.contains(body), "body が改変された: {doc}");
        // CSP はレスポンスヘッダで強制するため、文書内に meta CSP を入れてはならない（design doc 6章）。
        assert!(
            !doc.to_ascii_lowercase().contains("http-equiv"),
            "meta CSP を埋め込んだ（ヘッダ強制原則違反）: {doc}"
        );
        // このStageでは信頼 JS/ベンダーアセットを注入しない（Mermaid 等は後続Stage）。
        assert!(
            !doc.contains("<script"),
            "Stage ① でスクリプトを注入した: {doc}"
        );
    }

    #[test]
    fn 正常な外部許可ホストは_img_font_に反映される() {
        let allow = ExternalResourceAllow {
            hosts: vec!["https://cdn.example.com".to_string()],
        };
        let resp = prepare_markdown_preview("![x](rel.png)", &allow);
        assert!(
            resp.csp
                .contains("img-src pika-preview: https://cdn.example.com"),
            "正常な許可ホストが反映されない: {}",
            resp.csp
        );
    }
}
