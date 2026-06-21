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
/// セキュリティ上の不変条件（Stage ①/② で死守）:
/// - **`body` を改変しない**（[`sanitize_html`] 通過済みの本体をそのまま `<body>` 内へ埋め込む）。
/// - **`<meta http-equiv>` CSP を入れない**（CSP はレスポンスヘッダで強制する＝[`csp`]）。`<meta charset>` のみ。
/// - **注入する `<script>` は全て `nonce="{nonce}"` 付き**（外部 src も inline も）。CSP の
///   `script-src 'nonce-{nonce}'` と一致させ、`'unsafe-inline'`(script) は一切付けない（design doc 6章）。
///
/// Stage ②（系統A=[`PreviewFlavor::MarkdownTrustedJs`] のみ）:
/// - `<head>` に同梱 CSS（KaTeX/highlight）の `<link>` を入れる。KaTeX CSS 内の `url(fonts/KaTeX_*.woff2)`
///   は CSS の URL（`/assets/katex.min.css`）基準で `/assets/fonts/…` に解決される（font-src `'self'` 許可済み。
///   Windows の custom protocol 実オリジン `http://pika-preview.localhost` は `'self'` で一致する）。
/// - `<body>` 末尾に同梱ベンダー JS（highlight/katex/auto-render/mermaid）を nonce 付き `<script src>` で、
///   最後に [`TRUSTED_JS_INIT`]（per-block 描画・タイムアウト・危険オプション封じ）を inline `<script nonce>` で注入する。
/// - **条件付き注入（「軽い」原則・F-004 と整合）**: body を単純走査し、Mermaid/数式/コードブロックが
///   無ければ対応アセットを注入しない（未使用時のコストゼロ）。
///
/// 系統B（[`PreviewFlavor::HtmlNoJs`]）には **一切注入しない**（文書 JS 完全無効・従来どおり）。
///
/// 最小 base CSS（`<style>`）は読みやすさのためのみで、CSP の `style-src 'unsafe-inline'`（系統A/B とも許可）
/// の範囲に収まる。文書由来 CSS（系統B のインライン CSS）は `body` 側に既に含まれ、ここでは上書きしない。
pub fn wrap_preview_document(body: &str, nonce: &str, flavor: PreviewFlavor) -> String {
    // 最小 base CSS: 余白・行間・等幅・画像はみ出し抑制のみ（質感/テーマは後続Stageで CSS 変数受け渡し）。
    const BASE_CSS: &str = "\
html{color-scheme:light dark}\
body{margin:0;padding:16px;font-family:\"Segoe UI\",\"Meiryo\",\"Yu Gothic UI\",sans-serif;\
line-height:1.6;word-wrap:break-word;overflow-wrap:anywhere}\
img{max-width:100%;height:auto}\
pre{overflow:auto}\
table{border-collapse:collapse}\
th,td{border:1px solid currentColor;padding:.3em .6em}";

    // 系統A のみベンダーアセット注入の対象（系統B は文書 JS 完全無効）。
    // 条件付き注入（「軽い」原則・F-004）: 使われている機能だけ CSS link / JS を入れる。
    let assets = if matches!(flavor, PreviewFlavor::MarkdownTrustedJs) {
        detect_assets(body)
    } else {
        AssetNeeds::none()
    };

    let mut head_links = String::new();
    if assets.katex {
        // KaTeX CSS の url(fonts/...) は CSS の URL（/assets/katex.min.css）基準で /assets/fonts/... に解決される。
        head_links.push_str("<link rel=\"stylesheet\" href=\"/assets/katex.min.css\">\n");
    }
    if assets.highlight {
        head_links
            .push_str("<link rel=\"stylesheet\" href=\"/assets/hljs-github-dark.min.css\">\n");
    }

    let scripts = build_asset_scripts(&assets, nonce);

    format!(
        "<!DOCTYPE html>\n<html lang=\"ja\">\n<head>\n<meta charset=\"utf-8\">\n\
<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n\
{head_links}<style>{BASE_CSS}</style>\n</head>\n<body>\n{body}\n{scripts}</body>\n</html>\n"
    )
}

/// 系統A の body で使われている機能（どの同梱アセットを注入すべきか）。
struct AssetNeeds {
    mermaid: bool,
    katex: bool,
    highlight: bool,
}

impl AssetNeeds {
    /// 何も注入しない（系統B、または条件付き判定で全て不要だったとき）。
    fn none() -> Self {
        Self {
            mermaid: false,
            katex: false,
            highlight: false,
        }
    }

    /// どれか 1 つでも注入が必要か。
    fn any(&self) -> bool {
        self.mermaid || self.katex || self.highlight
    }
}

/// サニタイズ済み body を単純走査し、使われている機能を判定する（「軽い」原則・F-004 既存方針）。
///
/// 判定はサニタイズ済み body の文字列 contains のみ（過検出は無害＝余分に読み込むだけ・取りこぼしは
/// 描画されないだけで安全側に倒れる）。
/// - Mermaid: comrak 出力の `<code class="language-mermaid">`（`language-mermaid` を含むか）。
/// - 数式: KaTeX の区切り（`$` / `\(` / `\[`）が本文に出るか。
/// - コードハイライト: コードブロック（`<pre` または `<code class="language-`）が出るか。
fn detect_assets(body: &str) -> AssetNeeds {
    AssetNeeds {
        mermaid: body.contains("language-mermaid"),
        katex: body.contains('$') || body.contains("\\(") || body.contains("\\["),
        highlight: body.contains("<pre") || body.contains("<code class=\"language-"),
    }
}

/// 使われている機能の同梱ベンダー JS だけを nonce 付きで注入する body 末尾の `<script>` 群を組む。
///
/// 注入順序は body 末尾で highlight → katex → auto-render → mermaid → inline 初期化（[`TRUSTED_JS_INIT`]）。
/// すべて `nonce="{nonce}"` 付きで、CSP `script-src 'nonce-{nonce}'` に一致させる（design doc 6章）。
/// どの機能も使われていなければ初期化スクリプトすら注入しない（完全コストゼロ＝「軽い」原則）。
fn build_asset_scripts(assets: &AssetNeeds, nonce: &str) -> String {
    if !assets.any() {
        return String::new();
    }

    let mut out = String::new();
    if assets.highlight {
        out.push_str(&format!(
            "<script nonce=\"{nonce}\" src=\"/assets/highlight.min.js\"></script>\n"
        ));
    }
    if assets.katex {
        out.push_str(&format!(
            "<script nonce=\"{nonce}\" src=\"/assets/katex.min.js\"></script>\n\
<script nonce=\"{nonce}\" src=\"/assets/katex-auto-render.min.js\"></script>\n"
        ));
    }
    if assets.mermaid {
        out.push_str(&format!(
            "<script nonce=\"{nonce}\" src=\"/assets/mermaid.min.js\"></script>\n"
        ));
    }
    // 最後に inline 初期化（per-block 描画・約1秒タイムアウト・危険オプション封じ）を nonce 付きで注入する。
    // 各 run* 関数は対応ライブラリ未ロード時に即 return するため、条件付きで src を省いても安全。
    out.push_str(&format!(
        "<script nonce=\"{nonce}\">\n{TRUSTED_JS_INIT}\n</script>\n"
    ));
    out
}

/// 別WebView へ inline 注入する信頼 JS の初期化スクリプト本体（系統A・要件6.2・design doc 6章）。
///
/// **正本はここ（pika-core）**。frontend の旧 `buildTrustedJsInit`（`src/preview/index.ts`）は実行時の正本では
/// なくなった（Stage ② で Rust 側へ移植）。文字列の意味は旧実装と一致させる（per-block 描画・約1秒タイムアウト・
/// 失敗時の元コード復帰マーク・失敗件数集計）。
///
/// 危険オプション封じ（design doc 6章「信頼済み JS の危険オプション封じ」）:
/// - **Mermaid**: `securityLevel:'strict'`（click/任意 HTML 生成禁止）・`startOnLoad:false`（手動 per-block 描画）。
/// - **KaTeX**: `trust:false`・`strict:true`・`maxExpand:1000`・`throwOnError:false`（`\href{javascript:}`/`\htmlData` 経路封じ）。
/// - **highlight.js**: `pre code` のみ。
/// - 各ブロック個別描画。構文エラー/タイムアウト（約1秒）で当該ブロックを元コード表示へ戻しエラーマーク。
///
/// 失敗件数は `window.parent.postMessage` で通知するが、別WebView はトップレベルで `window.parent === window`
/// のため**無害な no-op**になる（Stage ③ で Tauri event/IPC へ置換予定。本 Stage は描画が主目的でそのままでよい）。
const TRUSTED_JS_INIT: &str = r#"(function(){
  "use strict";
  var BLOCK_TIMEOUT_MS = 1000;
  var failures = 0;
  function reportFailures(){
    if (failures > 0 && window.parent !== window) {
      try { window.parent.postMessage({ type: "pika-preview-failures", count: failures }, "*"); } catch(e){}
    }
  }
  function runHighlight(){
    if (!window.hljs) return;
    document.querySelectorAll("pre code").forEach(function(el){
      try { window.hljs.highlightElement(el); } catch(e){ failures++; }
    });
  }
  function runKatex(){
    if (!window.renderMathInElement) return;
    try {
      window.renderMathInElement(document.body, {
        delimiters: [
          { left: "$$", right: "$$", display: true },
          { left: "$", right: "$", display: false },
          { left: "\\(", right: "\\)", display: false },
          { left: "\\[", right: "\\]", display: true }
        ],
        trust: false, strict: true, maxExpand: 1000, throwOnError: false,
        errorCallback: function(){ failures++; }
      });
    } catch(e){ failures++; }
  }
  function runMermaid(){
    if (!window.mermaid) return;
    try { window.mermaid.initialize({ startOnLoad: false, securityLevel: "strict" }); } catch(e){}
    var blocks = document.querySelectorAll("pre code.language-mermaid, code.language-mermaid");
    blocks.forEach(function(el, i){
      var src = el.textContent || "";
      var host = document.createElement("div");
      host.className = "pika-mermaid";
      var id = "pika-mmd-" + i;
      var done = false;
      var timer = setTimeout(function(){ if(!done){ done = true; failures++; markError(el); } }, BLOCK_TIMEOUT_MS);
      try {
        window.mermaid.render(id, src).then(function(out){
          if (done) return; done = true; clearTimeout(timer);
          host.innerHTML = out.svg;
          var pre = el.closest("pre") || el;
          pre.parentNode.replaceChild(host, pre);
        }).catch(function(){ if(done) return; done = true; clearTimeout(timer); failures++; markError(el); });
      } catch(e){ if(!done){ done = true; clearTimeout(timer); failures++; markError(el); } }
    });
  }
  function markError(el){
    var pre = el.closest("pre") || el;
    if (pre && pre.classList) pre.classList.add("pika-block-error");
  }
  function init(){ runHighlight(); runKatex(); runMermaid(); setTimeout(reportFailures, BLOCK_TIMEOUT_MS + 50); }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();"#;

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
        assert!(resp.csp.contains("img-src 'self';"), "{}", resp.csp);
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
            resp.csp.contains("img-src 'self';"),
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
    fn wrap_系統aは使われた機能の_nonce_付き_script_link_を注入する() {
        // Stage ②: Mermaid/数式/コードを全て含む body には CSS link と全ベンダー JS が nonce 付きで入る。
        let body = "<pre><code class=\"language-mermaid\">graph TD;A--&gt;B</code></pre>\n\
<p>数式 $E=mc^2$ です</p>\n\
<pre><code class=\"language-rust\">fn main(){}</code></pre>";
        let doc = wrap_preview_document(body, "abc123", PreviewFlavor::MarkdownTrustedJs);
        // CSS link（KaTeX/highlight）。
        assert!(
            doc.contains("<link rel=\"stylesheet\" href=\"/assets/katex.min.css\">"),
            "KaTeX CSS link が無い: {doc}"
        );
        assert!(
            doc.contains("<link rel=\"stylesheet\" href=\"/assets/hljs-github-dark.min.css\">"),
            "highlight CSS link が無い: {doc}"
        );
        // ベンダー JS は全て nonce 付き src（外部 src も nonce 必須＝CSP 一致）。
        for src in [
            "/assets/highlight.min.js",
            "/assets/katex.min.js",
            "/assets/katex-auto-render.min.js",
            "/assets/mermaid.min.js",
        ] {
            assert!(
                doc.contains(&format!("<script nonce=\"abc123\" src=\"{src}\">")),
                "{src} の nonce 付き script が無い: {doc}"
            );
        }
        // inline 初期化も nonce 付き（per-block 描画・危険オプション封じ）。
        assert!(
            doc.contains("<script nonce=\"abc123\">"),
            "inline 初期化 script が nonce 付きで無い: {doc}"
        );
        // 危険オプション封じが文字列として乗っている（意味の回帰防止）。
        assert!(
            doc.contains("securityLevel: \"strict\""),
            "Mermaid strict が無い"
        );
        assert!(
            doc.contains("trust: false, strict: true, maxExpand: 1000"),
            "KaTeX 危険オプション封じが無い"
        );
        // nonce 無しの裸 inline script を絶対に入れない（CSP nonce 限定維持）。
        assert!(
            !doc.contains("<script>"),
            "nonce 無し script を注入した（CSP nonce 限定違反）: {doc}"
        );
        // body は一字一句改変しない（不変条件）。
        assert!(doc.contains(body), "body が改変された: {doc}");
        // meta CSP は入れない（ヘッダ強制原則）。
        assert!(
            !doc.to_ascii_lowercase().contains("http-equiv"),
            "meta CSP を埋め込んだ: {doc}"
        );
    }

    #[test]
    fn wrap_条件付き注入_使われない機能のアセットは入れない() {
        // Mermaid のみ使う body（数式・コードブロック無し）。katex/highlight は注入されない。
        let body = "<pre><code class=\"language-mermaid\">graph TD;A--&gt;B</code></pre>";
        let doc = wrap_preview_document(body, "n", PreviewFlavor::MarkdownTrustedJs);
        assert!(
            doc.contains("/assets/mermaid.min.js"),
            "mermaid が入っていない: {doc}"
        );
        // language-mermaid は `<pre`/`<code class="language-` を含むため highlight も注入される（過検出は無害）。
        // 数式区切り（$/\(/\[）は無いので KaTeX は注入されない。
        assert!(
            !doc.contains("/assets/katex.min.js"),
            "数式が無いのに KaTeX を注入した: {doc}"
        );
        assert!(
            !doc.contains("/assets/katex.min.css"),
            "数式が無いのに KaTeX CSS を注入した: {doc}"
        );
    }

    #[test]
    fn wrap_平易な本文には何も注入しない_コストゼロ() {
        // Mermaid/数式/コードブロックいずれも無い → ベンダー JS も初期化 script も注入しない。
        let body = "<h1>タイトル</h1>\n<p>ただの日本語段落です。</p>";
        let doc = wrap_preview_document(body, "n", PreviewFlavor::MarkdownTrustedJs);
        assert!(
            !doc.contains("<script"),
            "未使用なのに script を注入した（コストゼロ違反）: {doc}"
        );
        assert!(
            !doc.contains("/assets/"),
            "未使用なのにアセット link を注入した: {doc}"
        );
        assert!(doc.contains(body), "body が改変された: {doc}");
    }

    #[test]
    fn wrap_系統b_には一切注入しない() {
        // 系統B（HTML・JS 完全無効）にはコードブロックや数式があっても何も注入しない。
        let body = "<pre><code class=\"language-rust\">fn main(){}</code></pre>\n<p>$x$</p>";
        let doc = wrap_preview_document(body, "n", PreviewFlavor::HtmlNoJs);
        assert!(
            !doc.contains("<script"),
            "系統B に script を注入した（文書 JS 完全無効違反）: {doc}"
        );
        assert!(
            !doc.contains("/assets/"),
            "系統B にアセット link を注入した: {doc}"
        );
        assert!(doc.contains(body), "body が改変された: {doc}");
    }

    #[test]
    fn wrap_数式のみの本文には_katex_だけ入る() {
        // インラインコードを含まない純テキスト数式（highlight/mermaid は不要）。
        let body = "<p>方程式 $$a^2+b^2=c^2$$ について</p>";
        let doc = wrap_preview_document(body, "n", PreviewFlavor::MarkdownTrustedJs);
        assert!(
            doc.contains("/assets/katex.min.js"),
            "KaTeX が入っていない: {doc}"
        );
        assert!(
            doc.contains("/assets/katex.min.css"),
            "KaTeX CSS が入っていない: {doc}"
        );
        assert!(
            !doc.contains("/assets/mermaid.min.js"),
            "Mermaid が無いのに注入した: {doc}"
        );
        assert!(
            !doc.contains("/assets/highlight.min.js"),
            "コードブロックが無いのに highlight を注入した: {doc}"
        );
    }

    #[test]
    fn 正常な外部許可ホストは_img_font_に反映される() {
        let allow = ExternalResourceAllow {
            hosts: vec!["https://cdn.example.com".to_string()],
        };
        let resp = prepare_markdown_preview("![x](rel.png)", &allow);
        assert!(
            resp.csp.contains("img-src 'self' https://cdn.example.com"),
            "正常な許可ホストが反映されない: {}",
            resp.csp
        );
    }
}
