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
use std::borrow::Cow;
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
///
/// 系統B（HTML）で許可する CSS（`<style>` 本体・`style` 属性）は ammonia が値を検査しないため、
/// 多層防御が CSP 単層依存にならないよう CSS の危険トークン（`url(`/`@import`/`expression(`/
/// `image-set(`/`-moz-binding` 等の外部参照/危険関数）を [`sanitize_css_value`] / [`sanitize_style_element_body`]
/// で除去する（要件2.4「外部取得は既定オフ」を CSP だけに委ねない）。`style` 属性は
/// ammonia の `attribute_filter` で後処理し、`<style>` 本体はクリーン後の出力を後段で走査して落とす。
pub fn sanitize_html(html: &str, flavor: PreviewFlavor) -> String {
    let cleaned = build_sanitizer(flavor).clean(html).to_string();
    // <style> 要素本体（系統B のみ許可）の CSS は ammonia の属性検査では触れないため、
    // クリーン後の整形済み出力を走査して危険トークンを含む宣言/ルールを保守的に落とす。
    if flavor == PreviewFlavor::HtmlNoJs {
        sanitize_style_element_bodies(&cleaned)
    } else {
        cleaned
    }
}

/// CSS の値に外部参照/スクリプト誘発トークンが含まれるか（保守的判定・大小無視）。
///
/// 完全な CSS パーサは過大なので「危険トークンを含むなら宣言/ルールごと捨てる」保守的方針を採る
/// （取りこぼしより誤除去側に倒す＝多層防御の趣旨）。検出対象（要件2.4・CVE 系の経路封じ）:
/// - `url(` … 外部リソース取得（背景画像/フォント/cursor 等）。既定オフを CSS で復活させない。
/// - `@import` … 外部スタイルシート取り込み。
/// - `expression(` … IE 系の動的式（任意 JS 実行経路）。
/// - `image-set(` / `-webkit-image-set(` … 解像度別画像取得（url を含む外部取得）。
/// - `-moz-binding` … XBL バインディング（スクリプト実行経路）。
/// - `javascript:` … スキーム経由のスクリプト。
///
/// **CSS エスケープ回避対策（多層防御の第二層）**: ブラウザは `\75rl(` のような CSS 数値エスケープ
/// （`\75` = `u`）をデコードして `url(` として実効するため、素の substring 検査だけでは回避される。
/// 検査の前に [`decode_css_escapes`] で数値エスケープを実文字へデコードしてから判定する
/// （正当な `content:"\2022"`（•）等は外部取得トークンを形成しないため誤除去されない）。
fn css_has_dangerous_token(css: &str) -> bool {
    let decoded = decode_css_escapes(css);
    let lower = decoded.to_ascii_lowercase();
    const DANGEROUS: &[&str] = &[
        "url(",
        "@import",
        "expression(",
        "image-set(",
        "-moz-binding",
        "javascript:",
    ];
    DANGEROUS.iter().any(|t| lower.contains(t))
}

/// CSS の数値エスケープ（`\` + 16 進 1〜6 桁 + 任意の空白 1 つ）を実文字へデコードする
/// （危険トークン検査のエスケープ回避対策・多層防御の第二層）。
///
/// CSS 仕様の数値エスケープ（CSS Syntax Module §4.3.7）に従い、`\` の後に 16 進が 1〜6 桁続けば
/// その符号位置の文字へ置換し、エスケープ直後の空白 1 つは区切りとして消費する。`\` の後が 16 進でない
/// 場合（`\:` 等のリテラルエスケープ）は次の 1 文字をそのまま採る（コメント分断 `u/**/rl(` のような
/// url-token を形成しないケースは CSS 仕様上無害なので対象外）。
///
/// 目的は危険トークン（`url(`/`@import`/`expression(` 等）のエスケープ回避を検出することなので、
/// デコード結果を表示に使うわけではない（検査専用・誤除去を避けるため正当エスケープも忠実に復元する）。
fn decode_css_escapes(css: &str) -> String {
    let mut out = String::with_capacity(css.len());
    let mut chars = css.chars().peekable();
    while let Some(ch) = chars.next() {
        if ch != '\\' {
            out.push(ch);
            continue;
        }
        // バックスラッシュ。続く 16 進（最大 6 桁）を集める。
        let mut hex = String::new();
        while hex.len() < 6 {
            match chars.peek() {
                Some(&c) if c.is_ascii_hexdigit() => {
                    hex.push(c);
                    chars.next();
                }
                _ => break,
            }
        }
        if hex.is_empty() {
            // リテラルエスケープ（`\:` 等）。次の 1 文字をそのまま採る（無ければ `\` を落とす）。
            if let Some(c) = chars.next() {
                out.push(c);
            }
            continue;
        }
        // 数値エスケープ直後の空白 1 つは区切りとして消費する（CSS 仕様）。
        if matches!(chars.peek(), Some(c) if c.is_whitespace()) {
            chars.next();
        }
        // 16 進を符号位置へ。不正/範囲外は U+FFFD（仕様の置換文字）に倒す。
        let code = u32::from_str_radix(&hex, 16).unwrap_or(0xFFFD);
        out.push(char::from_u32(code).unwrap_or('\u{FFFD}'));
    }
    out
}

/// `style` 属性値を宣言（`;` 区切り）単位で保守的に検査し、危険トークンを含む宣言を捨てる。
///
/// 例: `color:red;background:url(http://evil/x.png)` → `color:red`（背景宣言だけ落とす）。
/// 全宣言が危険なら空文字を返す（呼び出し側＝`attribute_filter` が `style` 属性ごと除去する）。
fn sanitize_css_value(value: &str) -> String {
    value
        .split(';')
        .map(str::trim)
        .filter(|decl| !decl.is_empty() && !css_has_dangerous_token(decl))
        .collect::<Vec<_>>()
        .join(";")
}

/// クリーン済み HTML 内の `<style>...</style>` 本体を保守的に CSS サニタイズする。
///
/// ammonia は `<style>` 本体（テキストノード）を素通しするため、ここでクリーン後の整形済み出力から
/// `<style>` の中身を取り出し、宣言/ルール単位で危険トークン（`url(`/`@import`/`expression(` 等）を含む
/// ものを落とす。完全な CSS パーサは持たず、`;`/`}` 区切りの保守的分割で「危険なら捨てる」方針。
/// 本文中に `<style>` が無ければ入力をそのまま返す（系統B でのみ呼ばれる）。
fn sanitize_style_element_bodies(html: &str) -> String {
    const OPEN: &str = "<style>";
    const CLOSE: &str = "</style>";
    // ammonia 出力の <style> は属性を持たない（generic/tag_attributes に style 要素属性を許可していない）。
    if !html.contains(OPEN) {
        return html.to_string();
    }
    let mut out = String::with_capacity(html.len());
    let mut rest = html;
    while let Some(open_at) = rest.find(OPEN) {
        let body_start = open_at + OPEN.len();
        out.push_str(&rest[..body_start]);
        let after_open = &rest[body_start..];
        match after_open.find(CLOSE) {
            Some(close_rel) => {
                let css = &after_open[..close_rel];
                out.push_str(&sanitize_css_block(css));
                out.push_str(CLOSE);
                rest = &after_open[close_rel + CLOSE.len()..];
            }
            None => {
                // 閉じタグが無い（理論上 ammonia 出力では起きない）。残りをそのまま付けて終了。
                out.push_str(after_open);
                rest = "";
                break;
            }
        }
    }
    out.push_str(rest);
    out
}

/// `<style>` 本体の CSS を保守的にサニタイズする（ルール/宣言単位で危険トークンを捨てる）。
///
/// `@import url(...)` のような**ルール全体**が危険な場合と、`a{background:url(...)}` のように
/// ブロック内の一部宣言だけが危険な場合の両方を、`{`/`}`/`;` の素朴な区切りで保守的に処理する。
/// 取りこぼしより誤除去側に倒す（多層防御＝CSP に単層依存しないための念のための層）。
///
/// **ネストブロック（@media 等）の扱い**: `@media screen{.a{color:red}.b{background:url(x)}}` のような
/// ネスト本体は宣言列（`;` 区切り）ではなく内部の各ルールの集合なので、本体に `{` を含む（＝ネストルールを
/// 抱える）場合は [`sanitize_css_value`]（`;` 分割の宣言サニタイズ）ではなく本関数を**再帰適用**して
/// 内部ルールを個別にサニタイズする。これにより危険な `.b{background:url(x)}` だけを落とし、安全な
/// `.a{color:red}` を温存できる（従来は本体全体が 1 宣言扱いになり安全ルールごと黙って失われていた）。
///
/// **再帰深さ上限（DoS 対策）**: ネストブロックは [`sanitize_css_block_depth`] へ深さを引き継いで再帰する。
/// 未信頼 HTML の `<style>` に `a{a{a{…}}}` のような過大ネストを仕込まれるとスタックを食い潰し
/// プロセス abort（STATUS_STACK_BUFFER_OVERRUN）し得るため、[`CSS_MAX_NEST_DEPTH`] を超えたネスト本体は
/// 再帰せず**破棄する**（安全側＝誤除去に倒す既存方針と整合）。
fn sanitize_css_block(css: &str) -> String {
    sanitize_css_block_depth(css, 0)
}

/// CSS ネストブロックの最大再帰深さ。これを超えるネスト本体は破棄する（DoS 対策の fail-closed）。
/// 正当な CSS（@media の 1 段、@supports と @media の入れ子など）は浅く、32 段で十分余裕がある。
const CSS_MAX_NEST_DEPTH: usize = 32;

/// [`sanitize_css_block`] の実体。`depth` は現在の再帰深さ（最外は 0）。
fn sanitize_css_block_depth(css: &str, nest_depth: usize) -> String {
    let mut out = String::with_capacity(css.len());
    let mut buf = String::new(); // セレクタ/at-rule のプレリュード蓄積。
    let mut depth = 0usize;
    let mut chars = css.chars().peekable();
    while let Some(ch) = chars.next() {
        match ch {
            '{' if depth == 0 => {
                // セレクタ部に危険トークン（@import 等）があればこのルールを丸ごと捨てる。
                let prelude_dangerous = css_has_dangerous_token(&buf);
                // ブロック本体を収集する（ネストは @media 等。素朴に深さで対応）。
                let mut block = String::new();
                let mut block_has_nested = false; // 本体に `{` を含む＝@media 等のネストブロック。
                depth = 1;
                for inner in chars.by_ref() {
                    match inner {
                        '{' => {
                            depth += 1;
                            block_has_nested = true;
                            block.push(inner);
                        }
                        '}' => {
                            depth -= 1;
                            if depth == 0 {
                                break;
                            }
                            block.push(inner);
                        }
                        _ => block.push(inner),
                    }
                }
                if !prelude_dangerous {
                    // ネストブロック（@media 等）は内部の各ルールを再帰サニタイズし、危険ルールだけ落として
                    // 安全ルールを温存する。平坦ブロック（セレクタ{宣言;宣言}）は従来通り宣言単位で落とす。
                    let sanitized_block = if block_has_nested {
                        // 深さ上限を超えるネスト本体は再帰せず破棄する（スタック食い潰し DoS の防止）。
                        // ネストブロックを丸ごと落とすため、ここでルール自体を出力しないことで安全側に倒す。
                        if nest_depth + 1 > CSS_MAX_NEST_DEPTH {
                            buf.clear();
                            continue;
                        }
                        sanitize_css_block_depth(&block, nest_depth + 1)
                    } else {
                        sanitize_css_value(&block)
                    };
                    out.push_str(buf.trim());
                    out.push('{');
                    out.push_str(&sanitized_block);
                    out.push('}');
                }
                buf.clear();
            }
            ';' if depth == 0 => {
                // ブロック外の文（`@import ...;` 等）。危険トークンを含むなら捨てる。
                if !css_has_dangerous_token(&buf) {
                    let trimmed = buf.trim();
                    if !trimmed.is_empty() {
                        out.push_str(trimmed);
                        out.push(';');
                    }
                }
                buf.clear();
            }
            _ => buf.push(ch),
        }
    }
    // 末尾の余り（閉じられていない断片）は危険トークンが無いときだけ残す。
    if !buf.trim().is_empty() && !css_has_dangerous_token(&buf) {
        out.push_str(buf.trim());
    }
    out
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

    // style 属性（系統B のみ許可）の CSS 値を後処理し、外部参照/危険関数を含む宣言を捨てる
    // （ammonia は CSS 値を検査しない＝多層防御を CSP 単層に依存させない・要件2.4）。
    // 系統A は style 属性を許可しないため、このフィルタは系統A の出力に影響しない。
    if flavor == PreviewFlavor::HtmlNoJs {
        builder.attribute_filter(|_element, attribute, value| {
            if attribute == "style" {
                let safe = sanitize_css_value(value);
                if safe.is_empty() {
                    // 全宣言が危険＝style 属性ごと除去する。
                    None
                } else {
                    Some(Cow::Owned(safe))
                }
            } else {
                Some(Cow::Borrowed(value))
            }
        });
    }

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

    // SVG サブセット属性（要件6.2・design doc 6章）。描画に必要な幾何/塗り属性のみ。on* は一切含めない。
    // 注記（#41・実態との整合）: `<use>`/`<image>` の `href`/`xlink:href` は許可属性に**含めない**ため、
    // ammonia が属性ごと除去する（URL スキーム検査で弾くのではなく、そもそも属性が通らない）。
    // よって外部参照/`javascript:` も到達不能（許可しない方が単純で安全＝多層防御）。
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
    fn 系統b_style属性の_url_は除去される_csp単層依存しない() {
        // ammonia は CSS 値を検査しないため、属性フィルタで url() を含む宣言を落とす（要件2.4・多層防御）。
        let out = sanitize_html(
            r#"<div style="color:red;background:url(http://evil/x.png)">x</div>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(!out.contains("url("), "style 属性の url() が残った: {out}");
        assert!(!out.contains("evil"), "外部参照が残った: {out}");
        // 危険でない宣言（color:red）は保持される。
        assert!(out.contains("color:red"), "安全な宣言まで落ちた: {out}");
    }

    #[test]
    fn 系統b_style属性が全て危険なら属性ごと除去される() {
        let out = sanitize_html(
            r#"<div style="background:url(http://evil/x.png)">x</div>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(!out.contains("style="), "style 属性が残った: {out}");
        assert!(!out.contains("url("), "url() が残った: {out}");
    }

    #[test]
    fn 系統b_style属性の_expression_と_image_set_は除去される() {
        let out = sanitize_html(
            r#"<p style="color:blue;width:expression(alert(1))">x</p>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(!out.contains("expression("), "expression() が残った: {out}");
        assert!(out.contains("color:blue"), "安全な宣言まで落ちた: {out}");
        let out2 = sanitize_html(
            r#"<p style="background-image:image-set(url(a) 1x)">x</p>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(!out2.contains("image-set("), "image-set() が残った: {out2}");
    }

    #[test]
    fn 系統b_style要素本体の_import_は除去される() {
        // <style>@import url(...)</style> は外部 CSS 取得経路。ルールごと落とす。
        let out = sanitize_html(
            r#"<style>@import url("http://evil/x.css");.a{color:red}</style><p>x</p>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(out.contains("<style"), "<style> 要素自体は残る: {out}");
        assert!(!out.contains("@import"), "@import が残った: {out}");
        assert!(!out.contains("evil"), "外部 CSS 参照が残った: {out}");
        // 危険でないルール（.a{color:red}）は保持される。
        assert!(out.contains("color:red"), "安全なルールまで落ちた: {out}");
    }

    #[test]
    fn 系統b_style要素本体の宣言内_url_は宣言だけ落とす() {
        // ブロック内の一部宣言だけが危険な場合、そのブロックは残し危険宣言のみ捨てる。
        let out = sanitize_html(
            r#"<style>.b{color:green;background:url(http://evil/bg.png)}</style>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(!out.contains("url("), "宣言内 url() が残った: {out}");
        assert!(!out.contains("evil"), "外部参照が残った: {out}");
        assert!(out.contains("color:green"), "安全な宣言まで落ちた: {out}");
    }

    #[test]
    fn 系統b_media内の安全なルールは温存し危険なルールだけ除去する() {
        // 回帰修正: @media 等のネスト本体を `sanitize_css_value`（`;` 分割）に丸ごと渡すと、本体は `;` を
        // 含まないため 1 宣言扱いになり、url( を検出して安全な .a{color:red} ごと黙って失われていた。
        // ネスト本体は再帰サニタイズして危険な .b{background:url(x)} だけ落とし .a{color:red} を温存する。
        let out = sanitize_html(
            r#"<style>@media screen{.a{color:red}.b{background:url(x)}}</style>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(out.contains("<style"), "<style> 要素自体は残る: {out}");
        assert!(out.contains("@media"), "@media ルール自体は残る: {out}");
        // 安全なルールは温存される。
        assert!(
            out.contains("color:red"),
            "安全な .a{{color:red}} まで落ちた: {out}"
        );
        // 危険トークン（url(）は除去される（セキュリティ後退厳禁）。
        assert!(!out.contains("url("), "@media 内の url( が残った: {out}");
        assert!(
            !out.contains("url(x)"),
            "@media 内の url(x) が残った: {out}"
        );
    }

    #[test]
    fn 系統b_media内の宣言一部だけ危険なら宣言だけ落としルールは残す() {
        // ネストルール内で一部宣言だけ危険な場合、そのルールは残し危険宣言のみ捨てる。
        let out = sanitize_html(
            r#"<style>@media print{.c{color:green;background:url(http://evil/x.png)}}</style>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(out.contains("@media"), "@media が残る: {out}");
        assert!(out.contains("color:green"), "安全な宣言まで落ちた: {out}");
        assert!(!out.contains("url("), "ネスト宣言内 url( が残った: {out}");
        assert!(!out.contains("evil"), "外部参照が残った: {out}");
    }

    #[test]
    fn css_深いネストでもスタックオーバーフローせず返る() {
        // DoS 回帰: 未信頼 HTML の <style> に `a{a{a{…}}}` を仕込むと 1 ネスト=1 再帰フレームで
        // スタックを食い潰し pika.exe が abort（STATUS_STACK_BUFFER_OVERRUN）していた。
        // 深さ上限（CSS_MAX_NEST_DEPTH）超は破棄し、クラッシュせず返ること。
        let depth = 100_000usize;
        let mut css = String::with_capacity(depth * 2 + 16);
        css.push_str("<style>");
        for _ in 0..depth {
            css.push_str("a{");
        }
        css.push_str("color:red");
        for _ in 0..depth {
            css.push('}');
        }
        css.push_str("</style>");
        // パニック/abort せず返ればよい（出力内容は安全側に破棄されていてよい）。
        let out = sanitize_html(&css, PreviewFlavor::HtmlNoJs);
        // 上限超のネストは破棄されるため、最深の color:red は出力に残らない（安全側）。
        assert!(
            !out.contains("color:red"),
            "深い過大ネストの中身が温存された（破棄されるべき）: 末尾={}",
            &out[out.len().saturating_sub(60)..]
        );
    }

    #[test]
    fn css_正当な浅いネストは温存される() {
        // 上限導入で正当な @media（1 段）/@supports と @media の入れ子（2 段）が壊れないこと。
        let out = sanitize_html(
            r#"<style>@supports (display:grid){@media screen{.a{color:red}}}</style>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(out.contains("@supports"), "@supports が落ちた: {out}");
        assert!(out.contains("@media"), "@media が落ちた: {out}");
        assert!(out.contains("color:red"), "安全な宣言が落ちた: {out}");
    }

    #[test]
    fn css_数値エスケープでの危険トークン回避を検出する() {
        // ブラウザは `\75rl(` を `url(` にデコードして実効するため、検査前にデコードして弾く（多層防御の第二層）。
        // `\75` = u（url のエスケープ回避）。
        let out = sanitize_html(
            r#"<div style="background:\75rl(http://evil/x.png)">x</div>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(
            !out.contains("75rl") && !out.contains("evil"),
            "CSS エスケープ url() 回避が残った: {out}"
        );
        // `\40 import` = @import のエスケープ回避（直後の空白は区切り）。
        let out2 = sanitize_html(
            r#"<style>\40 import url("http://evil/x.css");.ok{color:red}</style>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(
            !out2.contains("40 import") && !out2.contains("evil"),
            "CSS エスケープ @import 回避が残った: {out2}"
        );
        assert!(out2.contains("color:red"), "安全なルールまで落ちた: {out2}");
        // `\65 xpression` = expression のエスケープ回避。
        let out3 = sanitize_html(
            r#"<div style="width:\65 xpression(alert(1))">x</div>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(
            !out3.contains("65 xpression"),
            "CSS エスケープ expression() 回避が残った: {out3}"
        );
    }

    #[test]
    fn css_の無害な数値エスケープは過剰に壊さない() {
        // content の `\2022`（•・bullet）は url-token を形成しない無害なエスケープ＝宣言を保持する。
        let out = sanitize_html(
            r#"<style>.q::before{content:"\2022";color:blue}</style>"#,
            PreviewFlavor::HtmlNoJs,
        );
        assert!(
            out.contains("\\2022") || out.contains("content"),
            "無害な content エスケープまで落ちた: {out}"
        );
        assert!(out.contains("color:blue"), "無害な宣言まで落ちた: {out}");
    }

    #[test]
    fn decode_css_escapes_は数値とリテラルを正しく展開する() {
        // 単体回帰: 数値エスケープ（直後空白消費含む）とリテラルエスケープ。
        assert_eq!(decode_css_escapes(r"\75rl("), "url(");
        assert_eq!(decode_css_escapes(r"\40 import"), "@import");
        assert_eq!(decode_css_escapes(r"\65 xpression"), "expression");
        assert_eq!(decode_css_escapes(r"\2022"), "\u{2022}"); // •
        assert_eq!(decode_css_escapes(r"\:"), ":"); // リテラルエスケープ。
        assert_eq!(decode_css_escapes("plain"), "plain"); // エスケープ無しは素通し。
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
