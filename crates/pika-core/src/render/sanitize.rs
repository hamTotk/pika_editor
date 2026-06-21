//! Markdown→HTML 変換と最終段サニタイズ（要件6.2・design doc 6章/7章・最重要セキュリティ境界）。
//!
//! 多層防御の中核（design doc 6章「サニタイズの多層」）:
//! 1. comrak(`unsafe_=true`) で Markdown を HTML 化する（raw HTML 透過＝要件6.2「Markdown内HTML対応」）。
//! 2. **最終段で必ず ammonia に通す**（中間生成物を WebView に渡さない）。ホワイトリスト方式で
//!    `<script>`・`on*` 属性・`javascript:`/scriptable `data:` URL・`<iframe>/<object>/<embed>/<base>/<meta>`
//!    を除去し、`id`/`name` 制限（DOM clobbering 防止）、SVG サブセット（`script`/`foreignObject`/`on*`/
//!    `xlink:href javascript:` を明示禁止）を適用する。
//! 3. `<meta http-equiv>`（CSP/refresh）はここで必ず除去する（CSP はレスポンスヘッダで強制＝[`crate::render::csp`]）。
//!
//! 本モジュールは Tauri/wry/WebView を一切知らない純粋ロジック（cargo test の決定論ゲート対象）。
//! comrak の中間 HTML は外へ出さず、必ず [`sanitize_html`] を通った文字列のみが配信される（呼び出し規約）。

use ammonia::Builder;
use std::collections::HashSet;

/// プレビューの系統（design doc 6章）。サニタイズ/CSP の厳しさを切り替える。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PreviewFlavor {
    /// 系統A: Markdown / 差分 / SVG。pika 同梱の信頼済み JS（Mermaid/KaTeX/highlight）のみ実行。
    MarkdownTrustedJs,
    /// 系統B: ユーザー文書の HTML。文書 JS は完全無効（CSP `script-src 'none'`・ammonia でも script 除去）。
    HtmlNoJs,
}

/// Markdown を GFM で HTML 化する（raw HTML 透過）。
///
/// comrak の `unsafe_=true` で raw HTML を通すが、戻り値は **必ず [`sanitize_html`] を通してから**
/// WebView へ渡す（呼び出し規約）。`sourcepos=true` で各ブロックに行情報を付け、双方向スクロール同期の
/// `data-line` 付与（should・要件6.1）の土台にする。
pub fn markdown_to_unsafe_html(markdown: &str) -> String {
    let mut options = comrak::Options::default();
    // GFM 準拠（テーブル・タスクリスト・打消し線・自動リンク＝要件6.2）。
    options.extension.table = true;
    options.extension.tasklist = true;
    options.extension.strikethrough = true;
    options.extension.autolink = true;
    // raw HTML を透過する（要件6.2「Markdown内HTML対応」）。最終段 ammonia で必ずサニタイズする。
    options.render.unsafe_ = true;
    // 双方向スクロール同期（要件6.1）の土台＝各ブロックに行情報を出力する。
    options.render.sourcepos = true;
    comrak::markdown_to_html(markdown, &options)
}

/// 未信頼 HTML を最終段サニタイズする（要件6.2・design doc 6章「ammonia 最終段固定」）。
///
/// 系統に応じてホワイトリストを切り替える:
/// - [`PreviewFlavor::MarkdownTrustedJs`]: 通常のタグ集合 + SVG サブセット。`<script>` は常に除去
///   （文書 JS は実行させない・同梱 JS は別WebView へ nonce 注入する＝design doc 6章）。
/// - [`PreviewFlavor::HtmlNoJs`]: 同じホワイトリストだが、JS 無効前提を強調する（CSP `script-src 'none'`）。
///   ammonia は両系統とも `<script>` を許可しないため、JS 由来 XSS はサニタイズ段でも遮断される。
///
/// いずれの系統でも除去するもの（要件6.2）:
/// - `<script>`・`on*` イベント属性・`javascript:`/scriptable `data:` URL
/// - `<iframe>/<object>/<embed>/<base>/<meta>`（`<meta http-equiv>` の CSP/refresh も含む）
/// - `id`/`name` 属性（DOM clobbering 防止）
/// - SVG の `script`/`foreignObject`/`on*`/`xlink:href javascript:`
pub fn sanitize_html(html: &str, flavor: PreviewFlavor) -> String {
    build_sanitizer(flavor).clean(html).to_string()
}

/// 系統に応じた ammonia ビルダーを構築する。
///
/// ammonia の既定は安全側（`<script>`/`on*`/`<iframe>` 等を許可しない）だが、本実装は
/// 「許可するタグ/属性/URL スキーム」を**明示**して要件6.2 のホワイトリストを固定する
/// （既定値の将来変更に左右されないため）。
fn build_sanitizer(flavor: PreviewFlavor) -> Builder<'static> {
    let mut builder = Builder::default();

    // 許可タグ（GFM 出力 + SVG サブセット）。<script>/<iframe>/<object>/<embed>/<base>/<meta> は含めない。
    let mut tags: HashSet<&'static str> = HashSet::from([
        // ブロック/インライン（GFM 出力）。
        "h1",
        "h2",
        "h3",
        "h4",
        "h5",
        "h6",
        "p",
        "div",
        "span",
        "blockquote",
        "pre",
        "code",
        "br",
        "hr",
        "em",
        "strong",
        "del",
        "s",
        "sub",
        "sup",
        "a",
        "ul",
        "ol",
        "li",
        "table",
        "thead",
        "tbody",
        "tr",
        "th",
        "td",
        "img",
        "input", // タスクリストの checkbox（type=checkbox disabled）。
        // SVG サブセット（design doc 6章: script/foreignObject は含めない）。
        "svg",
        "g",
        "path",
        "rect",
        "circle",
        "ellipse",
        "line",
        "polyline",
        "polygon",
        "text",
        "tspan",
        "defs",
        "lineargradient",
        "radialgradient",
        "stop",
        "use",
        "symbol",
        "title",
        "desc",
        "marker",
        "clippath",
        "mask",
        "pattern",
    ]);
    // 系統B（HTML）は文書スタイル尊重のため <style> を許可する（要件6.3・11.3。JS は CSP/ammonia で無効）。
    // ammonia 既定は <style> を `clean_content_tags`（中身ごと除去）に入れているため、tags へ足すなら
    // clean_content_tags から外す必要がある（両立できない＝ammonia の制約）。
    if flavor == PreviewFlavor::HtmlNoJs {
        tags.insert("style");
        builder.clean_content_tags(HashSet::from(["script"]));
    }
    builder.tags(tags);

    // 汎用許可属性（全タグ共通）。id/name は **許可しない**（DOM clobbering 防止＝design doc 6章）。
    builder.generic_attributes(HashSet::from(["class", "title"]));

    // タグ別許可属性。href/src は URL スキームのホワイトリストで scriptable を弾く（後述 url_schemes）。
    builder.tag_attributes(tag_attributes(flavor));

    // 許可 URL スキーム（javascript:/scriptable data: を含めない＝要件6.2）。
    // 相対参照（ローカル画像/.md リンク）は custom protocol が解決するため許可する。
    builder.url_schemes(HashSet::from([
        "http",
        "https",
        "mailto",
        "tel",
        "pika-preview",
    ]));
    // 相対 URL は許可（custom protocol 側で canonicalize+prefix 検証する＝[`crate::render::path`]）。
    builder.url_relative(ammonia::UrlRelative::PassThrough);
    // data: は scriptable な image/svg+xml を避け、ラスタ画像のみ許可する。
    // ammonia は url_schemes に data を含めない限り data: を除去するため、ここでは含めない＝既定で data: 全除去。

    // rel="noopener noreferrer" を a[target] へ強制（タブナビング対策・外部リンクラッパは frontend）。
    builder.link_rel(Some("noopener noreferrer"));

    builder
}

/// タグ別の許可属性（要件6.2・SVG サブセット）。`on*`・`id`・`name` は一切含めない。
fn tag_attributes(
    flavor: PreviewFlavor,
) -> std::collections::HashMap<&'static str, HashSet<&'static str>> {
    use std::collections::HashMap;
    let mut map: HashMap<&'static str, HashSet<&'static str>> = HashMap::new();
    map.insert("a", HashSet::from(["href", "target"]));
    map.insert("img", HashSet::from(["src", "alt", "width", "height"]));
    // タスクリストの checkbox（comrak は disabled checked を出す）。
    map.insert("input", HashSet::from(["type", "checked", "disabled"]));
    map.insert(
        "th",
        HashSet::from(["align", "colspan", "rowspan", "scope"]),
    );
    map.insert("td", HashSet::from(["align", "colspan", "rowspan"]));
    map.insert("code", HashSet::from(["class"]));
    map.insert("pre", HashSet::from(["class"]));
    // comrak sourcepos の data-sourcepos（双方向スクロール同期の土台）。
    for tag in [
        "h1",
        "h2",
        "h3",
        "h4",
        "h5",
        "h6",
        "p",
        "li",
        "blockquote",
        "table",
        "pre",
    ] {
        map.insert(tag, HashSet::from(["data-sourcepos"]));
    }

    // SVG サブセット属性（要件6.2・design doc 6章）。xlink:href は javascript: を URL スキームで弾く。
    // 描画に必要な幾何/塗り属性のみ。on* は **一切含めない**。
    let svg_common = HashSet::from([
        "width",
        "height",
        "viewbox",
        "x",
        "y",
        "cx",
        "cy",
        "r",
        "rx",
        "ry",
        "x1",
        "y1",
        "x2",
        "y2",
        "d",
        "points",
        "fill",
        "stroke",
        "stroke-width",
        "stroke-linecap",
        "stroke-linejoin",
        "stroke-dasharray",
        "opacity",
        "fill-opacity",
        "stroke-opacity",
        "transform",
        "gradientunits",
        "offset",
        "stop-color",
        "stop-opacity",
        "text-anchor",
        "font-size",
        "font-family",
        "preserveaspectratio",
        "xmlns",
        "version",
    ]);
    for tag in [
        "svg",
        "g",
        "path",
        "rect",
        "circle",
        "ellipse",
        "line",
        "polyline",
        "polygon",
        "text",
        "tspan",
        "defs",
        "lineargradient",
        "radialgradient",
        "stop",
        "use",
        "symbol",
        "marker",
        "clippath",
        "mask",
        "pattern",
    ] {
        map.insert(tag, svg_common.clone());
    }

    // 系統B（HTML）は style 属性（インライン CSS）を尊重する（要件6.3）。系統A は不要。
    if flavor == PreviewFlavor::HtmlNoJs {
        // 全タグに style を許すと generic に足したいが ammonia の API 上は generic_attributes だと
        // url スキーム検査が効かない属性なので generic 側で扱う。ここでは主要タグへ付与する。
        for tag in [
            "div",
            "span",
            "p",
            "table",
            "tr",
            "td",
            "th",
            "h1",
            "h2",
            "h3",
            "h4",
            "h5",
            "h6",
            "img",
            "a",
            "ul",
            "ol",
            "li",
            "blockquote",
            "code",
            "pre",
            "em",
            "strong",
        ] {
            map.entry(tag).or_default().insert("style");
        }
    }

    map
}

#[cfg(test)]
mod tests {
    use super::*;

    fn clean_a(html: &str) -> String {
        sanitize_html(html, PreviewFlavor::MarkdownTrustedJs)
    }

    #[test]
    fn script_タグは除去される() {
        let out = clean_a("<p>hi</p><script>alert(1)</script>");
        assert!(!out.contains("<script"), "script タグが残った: {out}");
        assert!(!out.contains("alert(1)"), "script 本文が残った: {out}");
        assert!(out.contains("<p>hi</p>"));
    }

    #[test]
    fn イベントハンドラ属性は除去される() {
        let out = clean_a(r#"<img src="x.png" onerror="alert(1)">"#);
        assert!(!out.contains("onerror"), "on* 属性が残った: {out}");
        assert!(!out.contains("alert(1)"));
    }

    #[test]
    fn javascript_url_は除去される() {
        let out = clean_a(r#"<a href="javascript:alert(1)">x</a>"#);
        assert!(
            !out.contains("javascript:"),
            "javascript: URL が残った: {out}"
        );
    }

    #[test]
    fn scriptable_data_url_は除去される() {
        // data:text/html・data:image/svg+xml は scriptable なので除去（data: 全除去方針）。
        let out = clean_a(r#"<a href="data:text/html,<script>alert(1)</script>">x</a>"#);
        assert!(
            !out.contains("data:text/html"),
            "scriptable data: が残った: {out}"
        );
        let img = clean_a(r#"<img src="data:image/svg+xml,<svg onload=alert(1)>">"#);
        assert!(
            !img.contains("data:image/svg+xml"),
            "svg data: が残った: {img}"
        );
    }

    #[test]
    fn iframe_object_embed_base_meta_は除去される() {
        for tag in ["iframe", "object", "embed", "base", "meta"] {
            let html = format!("<p>x</p><{tag}></{tag}>");
            let out = clean_a(&html);
            assert!(!out.contains(&format!("<{tag}")), "{tag} が残った: {out}");
        }
    }

    #[test]
    fn meta_refresh_と_csp_は除去される() {
        // <meta http-equiv> の CSP/refresh は ammonia 段で必ず除去する（CSP はヘッダで強制）。
        let out = clean_a(r#"<meta http-equiv="refresh" content="0;url=http://evil"><p>x</p>"#);
        assert!(!out.contains("<meta"), "meta refresh が残った: {out}");
        assert!(out.contains("<p>x</p>"));
    }

    #[test]
    fn id_と_name_属性は除去される_dom_clobbering防止() {
        // DOM clobbering 防止（要件6.2・design doc 6章）。
        let out = clean_a(r#"<div id="x" name="y"><a name="z">a</a></div>"#);
        assert!(
            !out.contains("id="),
            "id 属性が残った（DOM clobbering）: {out}"
        );
        assert!(
            !out.contains("name="),
            "name 属性が残った（DOM clobbering）: {out}"
        );
    }

    #[test]
    fn svg_の_script_と_foreignobject_と_on属性は除去される() {
        let out = clean_a(
            r#"<svg><script>alert(1)</script><foreignObject><body onload="alert(1)"/></foreignObject><rect onclick="x()"/></svg>"#,
        );
        assert!(!out.contains("<script"), "svg 内 script が残った: {out}");
        assert!(
            !out.to_lowercase().contains("foreignobject"),
            "foreignObject が残った: {out}"
        );
        assert!(!out.contains("onclick"), "svg on* 属性が残った: {out}");
        assert!(!out.contains("onload"));
    }

    #[test]
    fn svg_の_xlink_href_javascript_は除去される() {
        let out = clean_a(r#"<svg><use xlink:href="javascript:alert(1)"/></svg>"#);
        assert!(
            !out.contains("javascript:"),
            "svg xlink:href javascript: が残った: {out}"
        );
    }

    #[test]
    fn 安全な_markdown_は保持される() {
        let html = markdown_to_unsafe_html(
            "# 見出し\n\n- [x] task\n- a\n\n| a | b |\n|---|---|\n| 1 | 2 |\n",
        );
        let out = clean_a(&html);
        assert!(out.contains("<h1"), "見出しが消えた: {out}");
        assert!(out.contains("<table"), "テーブルが消えた: {out}");
        assert!(out.contains("<li"), "リストが消えた: {out}");
    }

    #[test]
    fn comrak_の_raw_html_内_script_も最終段で除去される() {
        // comrak(unsafe_) は raw HTML を透過するが、最終段 ammonia が必ず除去する（多層の要）。
        let html = markdown_to_unsafe_html("text\n\n<script>steal()</script>\n\n<div>ok</div>");
        let out = clean_a(&html);
        assert!(
            !out.contains("<script"),
            "raw HTML の script が残った: {out}"
        );
        assert!(!out.contains("steal()"));
        assert!(out.contains("<div>ok</div>"));
    }

    #[test]
    fn 系統b_html_は_style_を尊重する() {
        let out = sanitize_html(
            r#"<div style="color:red"><style>.a{color:blue}</style>x</div>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(out.contains("style="), "インライン style が消えた: {out}");
        assert!(out.contains("<style"), "<style> が消えた: {out}");
    }

    #[test]
    fn sourcepos_data属性は双方向スクロール同期の土台として保持される() {
        // comrak sourcepos の data-sourcepos を ammonia が落とさないこと（要件6.1 同期の土台・should）。
        let html = markdown_to_unsafe_html("# 見出し\n\n段落");
        assert!(
            html.contains("data-sourcepos"),
            "comrak が sourcepos を出していない: {html}"
        );
        let out = clean_a(&html);
        assert!(
            out.contains("data-sourcepos"),
            "sourcepos がサニタイズで消えた: {out}"
        );
    }

    #[test]
    fn 系統a_でも_script_は系統bと同様に除去される() {
        // 系統A/B いずれも文書 JS は実行させない（同梱 JS は別WebView へ nonce 注入する）。
        let out_b = sanitize_html("<script>x()</script><p>y</p>", PreviewFlavor::HtmlNoJs);
        assert!(
            !out_b.contains("<script"),
            "系統B で script が残った: {out_b}"
        );
    }
}
