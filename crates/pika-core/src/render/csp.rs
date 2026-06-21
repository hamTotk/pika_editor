//! CSP レスポンスヘッダの組立（要件6.2/6.3・design doc 6章「CSP はレスポンスヘッダで強制」）。
//!
//! CSP は custom protocol レスポンスの HTTP ヘッダで返す（文書内 `<meta http-equiv>` は
//! [`crate::render::sanitize`] 段で除去する＝文書依存にしない）。系統で厳しさを切り替える:
//!
//! - **系統A（Markdown/差分/SVG）**: 同梱信頼 JS のみ実行＝`script-src 'nonce-<rnd>'`。
//! - **系統B（HTML）**: 文書 JS 完全無効＝`script-src 'none'`。
//!
//! **オプトイン緩和は `img-src`/`font-src` への許可ホスト追加に限定**し、`script-src`/`connect-src`/
//! `object-src` は緩めない。許可は **必ず既定（外部遮断）に戻る**（要件6.2/6.3・2.4 のプライバシー方針）。
//!
//! 本モジュールは Tauri/wry を一切知らない純粋ロジック（cargo test の決定論ゲート対象）。

use super::sanitize::PreviewFlavor;

/// CSP の nonce（系統A の `script-src 'nonce-<rnd>'` に使う）。
///
/// 別WebView へ注入する信頼済み JS（Mermaid/KaTeX/highlight）に同じ nonce を付与する。
/// `'unsafe-inline'`（script）は決して付けない（design doc 6章）。
pub type Nonce = String;

/// 暗号品質乱数から base64url 風の nonce を生成する（16 バイト＝128bit）。
///
/// CSP の nonce は推測不能であることが要件（攻撃者が nonce を予測できると注入が成立する）。
/// レスポンスごとに新規生成し、同一文書内の信頼 JS にのみ付与する。
pub fn generate_nonce() -> Nonce {
    let mut bytes = [0u8; 16];
    // 失敗時はパニックさせず固定でない値にフォールバックしない（推測可能 nonce は付けない）。
    // getrandom はプラットフォーム CSPRNG。失敗は事実上起きないが、起きたら nonce 無効化のため空を返さず panic。
    getrandom::getrandom(&mut bytes).expect("CSPRNG から nonce を生成できませんでした");
    encode_base64url(&bytes)
}

/// 外部リソースのオプトイン許可（要件6.2/6.3）。**既定は空＝外部遮断**。
///
/// 緩和できるのは `img-src`/`font-src` の許可ホストのみ（design doc 6章）。
/// `script-src`/`connect-src`/`object-src` は対象外（構造的に緩められない）。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ExternalResourceAllow {
    /// 許可する外部 img/font ホスト（例: `"https://example.com"`）。空＝外部遮断（既定）。
    pub hosts: Vec<String>,
}

impl ExternalResourceAllow {
    /// 既定（外部遮断）。許可ホストなし。
    pub fn blocked() -> Self {
        Self { hosts: Vec::new() }
    }
}

/// オプトイン外部許可ホストを検証する（CSP ディレクティブインジェクション防止・要件6.2/6.3）。
///
/// `build_csp` は許可ホストを `img-src`/`font-src` へ文字列連結する。ホスト文字列に空白や
/// `;`/クォート/改行が混入すると CSP に別ディレクティブ（例 `; script-src *`）を追記でき、
/// nonce 限定ポリシーを破る余地が生まれる（最重要 XSS 面）。
/// そこで **CSP へ渡す前に必ず本関数で許可リストを検証**し、不正な要素が 1 つでもあれば
/// 全体を拒否する（[`build_csp`] へは検証済みのみ渡す呼び出し規約）。
///
/// 受理条件（design doc 6章「外部緩和は img/font の許可ホスト追加のみ」）:
/// - スキームは `https://` のみ（`http:`・`javascript:`・`data:` 等は不可＝プライバシー/盗聴防止）。
/// - ホスト名（とオプションのポート）は `A-Za-z0-9.-` ＋ ポート `:` 数字のみ。
/// - 空白・`;`・`'`・`"`・改行・`*`（ワイルドカード全許可）・パス/クエリ付きは拒否。
///
/// 戻り値: 全ホストが受理可能なら `Ok(())`、1 つでも不正なら `Err`（拒否理由）。
pub fn validate_allow_hosts(allow: &ExternalResourceAllow) -> Result<(), String> {
    for host in &allow.hosts {
        validate_one_host(host)
            .map_err(|reason| format!("不正な外部許可ホスト `{host}`: {reason}"))?;
    }
    Ok(())
}

/// 1 ホスト文字列を検証する（`validate_allow_hosts` の単体）。
fn validate_one_host(host: &str) -> Result<(), &'static str> {
    // スキームは https のみ。CSP の source は scheme を含めても素のホストでもよいが、本実装は
    // https:// 始まりを必須にして http:（盗聴）や scriptable scheme の混入余地を断つ。
    let rest = host
        .strip_prefix("https://")
        .ok_or("https:// で始まる必要があります")?;
    if rest.is_empty() {
        return Err("ホスト名が空です");
    }
    // パス/クエリ/フラグメント付きは拒否（source はホスト[:ポート]のみ許可）。
    if rest.contains('/') || rest.contains('?') || rest.contains('#') {
        return Err("パス/クエリは許可されません（ホスト[:ポート]のみ）");
    }
    // CSP ディレクティブ区切り・引用符・空白・改行・ワイルドカードの混入を拒否（注入防止）。
    if rest
        .chars()
        .any(|c| c.is_whitespace() || matches!(c, ';' | '\'' | '"' | '*' | ',' | '\\'))
    {
        return Err("空白/区切り/クォート/ワイルドカードを含みます");
    }
    // ホスト名[:ポート]の文字種を限定する（英数・`.`・`-`、ポートは `:` ＋数字）。
    let (hostname, port) = match rest.split_once(':') {
        Some((h, p)) => (h, Some(p)),
        None => (rest, None),
    };
    if hostname.is_empty() {
        return Err("ホスト名が空です");
    }
    if !hostname
        .chars()
        .all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '-')
    {
        return Err("ホスト名に不正な文字が含まれます");
    }
    if let Some(p) = port {
        if p.is_empty() || !p.chars().all(|c| c.is_ascii_digit()) {
            return Err("ポートは数字のみです");
        }
    }
    Ok(())
}

/// custom protocol レスポンスの CSP ヘッダ値を組み立てる（要件6.2・design doc 6章）。
///
/// - `flavor`: 系統A=信頼 JS（nonce）/ 系統B=JS 無効。
/// - `nonce`: 系統A の `script-src 'nonce-<rnd>'`。系統B では使われない。
/// - `allow`: オプトイン外部許可（img-src/font-src のみに反映。既定 [`ExternalResourceAllow::blocked`]）。
///
/// 固定方針（design doc 6章の確定 CSP）:
/// - `default-src 'none'`（既定全遮断）
/// - 系統A `script-src 'nonce-<rnd>'` / 系統B `script-src 'none'`
/// - `connect-src 'none'`・`object-src 'none'`・`base-uri 'none'`・`frame-ancestors 'none'`・`frame-src 'none'`
/// - `img-src`/`font-src` は `'self'` + 許可ホスト（許可が空なら `'self'` のみ＝同一オリジンのみ＝既定遮断）。
///   **同一オリジン資源は `'self'` で許可する**: Windows では custom protocol の実オリジンは
///   `http://pika-preview.localhost`（http スキーム・host=pika-preview.localhost）に解決されるため、
///   scheme-source `pika-preview:` は実オリジンに **マッチしない**（KaTeX CSS/フォント・ローカル画像が
///   ブロックされる）。`'self'` は実オリジン `http://pika-preview.localhost` に正しく一致する。
pub fn build_csp(flavor: PreviewFlavor, nonce: &str, allow: &ExternalResourceAllow) -> String {
    let script_src = match flavor {
        PreviewFlavor::MarkdownTrustedJs => format!("'nonce-{nonce}'"),
        // 系統B（HTML）は文書 JS 完全無効。
        PreviewFlavor::HtmlNoJs => "'none'".to_string(),
    };

    // img-src/font-src: 同一オリジン資源は 'self' で許可（ローカル相対参照解決）。許可ホストのみ追加できる。
    // Windows の custom protocol 実オリジンは `http://pika-preview.localhost`（http/host）に解決されるため、
    // scheme-source `pika-preview:` は実オリジンに一致せずローカル画像/フォント/CSS がブロックされる。
    // `'self'` は実オリジンと一致するため、系統A/B 共通でローカル資源が配信できる。
    // 呼び出し側は prepare_*_preview 経由で validate_allow_hosts 済みだが、CSP インジェクションは
    // 最重要 XSS 面のため **ここでも防御的に再検証**し、不正ホストは連結せず黙って捨てる
    // （多層防御＝検証漏れの呼び出し経路が後で増えてもポリシーを破らせない）。
    let mut img_font = String::from("'self'");
    for host in &allow.hosts {
        if validate_one_host(host).is_err() {
            continue;
        }
        img_font.push(' ');
        img_font.push_str(host);
    }

    // style-src（スタイルのみの緩和。script-src は一切触らない＝design doc 6章・最重要 XSS 面）。
    // 系統A:
    //   - 同梱 KaTeX/highlight の CSS は同一オリジンの <link> で入れるため source 許可する。
    //     Windows の custom protocol 実オリジンは `http://pika-preview.localhost` ＝ `'self'` で一致する。
    //     scheme-source `pika-preview:` は実オリジンに不一致のため不可（KaTeX CSS がブロックされ崩れる）。
    //   - **CSP3 では style-src に nonce/hash があると `'unsafe-inline'` が無効化される**。KaTeX はインライン
    //     style 属性を、Mermaid は SVG に <style> を JS 注入するため、nonce 併記だとこれらがブロックされ崩れる。
    //     よって nonce を併記せず `'unsafe-inline' 'self'`（インラインスタイルを通す＋同梱 CSS link）にする。
    // 系統B: 文書のインライン CSS 表現を許す `'unsafe-inline'`（従来どおり・変更なし）。
    // いずれも script-src には `'unsafe-inline'` を付けない（nonce 限定）。
    let style_src = match flavor {
        PreviewFlavor::MarkdownTrustedJs => "'unsafe-inline' 'self'".to_string(),
        PreviewFlavor::HtmlNoJs => "'unsafe-inline'".to_string(),
    };

    format!(
        "default-src 'none'; \
         script-src {script_src}; \
         style-src {style_src}; \
         img-src {img_font}; \
         font-src {img_font}; \
         connect-src 'none'; \
         frame-src 'none'; \
         object-src 'none'; \
         base-uri 'none'; \
         form-action 'none'; \
         frame-ancestors 'none'"
    )
}

/// `getrandom` のバイト列を base64url（パディングなし）にエンコードする。
///
/// nonce は ASCII であればよく、CSP の `'nonce-...'` に安全に入る文字集合（`-`/`_` を使う base64url）。
fn encode_base64url(bytes: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = *chunk.get(1).unwrap_or(&0) as u32;
        let b2 = *chunk.get(2).unwrap_or(&0) as u32;
        let n = (b0 << 16) | (b1 << 8) | b2;
        out.push(ALPHABET[((n >> 18) & 0x3f) as usize] as char);
        out.push(ALPHABET[((n >> 12) & 0x3f) as usize] as char);
        if chunk.len() > 1 {
            out.push(ALPHABET[((n >> 6) & 0x3f) as usize] as char);
        }
        if chunk.len() > 2 {
            out.push(ALPHABET[(n & 0x3f) as usize] as char);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    /// CSP 文字列から 1 ディレクティブの値（名前を除いた残り）を取り出す（テスト用）。
    /// 例: `extract_directive("default-src 'none'; script-src 'nonce-x'", "script-src")` → `"'nonce-x'"`。
    fn extract_directive<'a>(csp: &'a str, name: &str) -> &'a str {
        for part in csp.split(';') {
            let part = part.trim();
            if let Some(value) = part.strip_prefix(name) {
                // 直後が空白で区切られていることを確認（`script-src` が `script-src-attr` 等に誤マッチしない）。
                let value = value.trim_start();
                // strip_prefix は前方一致なので、name 完全一致のため value の前に空白があったはず。
                return value;
            }
        }
        panic!("CSP にディレクティブ `{name}` が無い: {csp}");
    }

    #[test]
    fn 系統a_は_nonce_スクリプトのみ許可する() {
        let csp = build_csp(
            PreviewFlavor::MarkdownTrustedJs,
            "abc123",
            &ExternalResourceAllow::blocked(),
        );
        assert!(csp.contains("script-src 'nonce-abc123'"), "{csp}");
        assert!(
            !csp.contains("'unsafe-inline'; script"),
            "script に unsafe-inline が混入: {csp}"
        );
        // script-src に 'unsafe-inline' を付けない（design doc 6章・最重要 XSS 面）。
        assert!(
            !csp.contains("script-src 'nonce-abc123' 'unsafe-inline'"),
            "{csp}"
        );
        // script-src は nonce 限定（'unsafe-inline'/'unsafe-eval'/外部スキームを一切含めない）。
        let script_src = extract_directive(&csp, "script-src");
        assert_eq!(
            script_src, "'nonce-abc123'",
            "script-src が nonce 限定でない: {csp}"
        );
    }

    #[test]
    fn 系統a_の_style_src_は_unsafe_inline_と同梱オリジン許可で_nonce_を併記しない() {
        // Stage ②: KaTeX のインライン style 属性・Mermaid の SVG <style> 注入を通すため、style-src は
        // 'unsafe-inline' を有効に保つ（CSP3 では nonce/hash 併記すると 'unsafe-inline' が無効化されるため
        // **nonce を併記しない**）。同梱 CSS の <link>（同一オリジン）は 'self' で許可する
        // （Windows の custom protocol 実オリジン `http://pika-preview.localhost` は 'self' で一致。
        //  scheme-source `pika-preview:` は不一致のため不可）。
        let csp = build_csp(
            PreviewFlavor::MarkdownTrustedJs,
            "abc123",
            &ExternalResourceAllow::blocked(),
        );
        let style_src = extract_directive(&csp, "style-src");
        assert!(
            style_src.contains("'unsafe-inline'"),
            "style-src に 'unsafe-inline' が無い（KaTeX/Mermaid のスタイルがブロックされる）: {csp}"
        );
        assert!(
            style_src.contains("'self'"),
            "style-src に同梱 CSS link 用の 'self' が無い: {csp}"
        );
        // scheme-source pika-preview: は実オリジン不一致のため使わない（リグレッション防止）。
        assert!(
            !style_src.contains("pika-preview:"),
            "style-src に実オリジン不一致の pika-preview: が残った: {csp}"
        );
        // nonce を style-src に併記しない（併記すると 'unsafe-inline' が無効化される＝CSP3）。
        assert!(
            !style_src.contains("nonce-"),
            "style-src に nonce を併記した（'unsafe-inline' が無効化され KaTeX/Mermaid が崩れる）: {csp}"
        );
        // style-src の緩和が script-src へ波及していないこと（最重要）。
        assert_eq!(
            extract_directive(&csp, "script-src"),
            "'nonce-abc123'",
            "style-src 変更が script-src に波及した: {csp}"
        );
    }

    #[test]
    fn 系統b_は_スクリプトを完全無効化する() {
        let csp = build_csp(
            PreviewFlavor::HtmlNoJs,
            "abc123",
            &ExternalResourceAllow::blocked(),
        );
        assert!(
            csp.contains("script-src 'none'"),
            "系統B で script-src 'none' でない: {csp}"
        );
        assert!(!csp.contains("nonce-"), "系統B に nonce が入った: {csp}");
    }

    #[test]
    fn 既定は外部全遮断() {
        let csp = build_csp(
            PreviewFlavor::MarkdownTrustedJs,
            "n",
            &ExternalResourceAllow::blocked(),
        );
        assert!(csp.contains("default-src 'none'"), "{csp}");
        assert!(csp.contains("connect-src 'none'"), "{csp}");
        assert!(csp.contains("object-src 'none'"), "{csp}");
        assert!(csp.contains("base-uri 'none'"), "{csp}");
        assert!(csp.contains("frame-ancestors 'none'"), "{csp}");
        // img/font は 'self'（同一オリジン）のみ（外部ホストなし）。
        // Windows の custom protocol 実オリジン `http://pika-preview.localhost` は 'self' で一致する。
        assert!(csp.contains("img-src 'self';"), "{csp}");
        assert!(csp.contains("font-src 'self';"), "{csp}");
        // scheme-source pika-preview: は実オリジン不一致のため使わない（リグレッション防止）。
        assert!(!csp.contains("pika-preview:"), "{csp}");
    }

    #[test]
    fn オプトイン緩和は_img_font_のみに反映する() {
        let allow = ExternalResourceAllow {
            hosts: vec!["https://example.com".to_string()],
        };
        let csp = build_csp(PreviewFlavor::MarkdownTrustedJs, "n", &allow);
        assert!(
            csp.contains("img-src 'self' https://example.com"),
            "img-src に許可ホストが反映されない: {csp}"
        );
        assert!(
            csp.contains("font-src 'self' https://example.com"),
            "font-src に許可ホストが反映されない: {csp}"
        );
        // script-src/connect-src/object-src は緩めない（許可ホストが混入しない）。
        assert!(
            !csp.contains("script-src 'nonce-n' https://example.com"),
            "{csp}"
        );
        assert!(
            csp.contains("connect-src 'none'"),
            "connect-src が緩んだ: {csp}"
        );
        assert!(
            csp.contains("object-src 'none'"),
            "object-src が緩んだ: {csp}"
        );
    }

    #[test]
    fn 緩和を空にすると既定の外部遮断に戻る() {
        // 「既定は必ずオフに戻る」（要件6.2/6.3）。
        let csp = build_csp(
            PreviewFlavor::MarkdownTrustedJs,
            "n",
            &ExternalResourceAllow::default(),
        );
        assert!(
            csp.contains("img-src 'self';"),
            "外部遮断に戻っていない: {csp}"
        );
        assert!(!csp.contains("https://"), "{csp}");
    }

    #[test]
    fn 許可ホスト検証_正常な_https_ホストは受理する() {
        let allow = ExternalResourceAllow {
            hosts: vec![
                "https://example.com".to_string(),
                "https://cdn.example.com:8443".to_string(),
                "https://a-b.example-cdn.net".to_string(),
            ],
        };
        assert!(validate_allow_hosts(&allow).is_ok());
    }

    #[test]
    fn 許可ホスト検証_csp_インジェクションを拒否する() {
        // セミコロンで script-src を追記する古典的 CSP インジェクション。
        let injection = ExternalResourceAllow {
            hosts: vec!["https://evil.com; script-src *".to_string()],
        };
        assert!(
            validate_allow_hosts(&injection).is_err(),
            "セミコロンによる CSP ディレクティブ追記が通った"
        );
        // build_csp も防御的に不正ホストを連結しない（多層防御）。
        let csp = build_csp(PreviewFlavor::MarkdownTrustedJs, "n", &injection);
        assert!(
            !csp.contains("script-src *"),
            "不正ホストが CSP に連結された: {csp}"
        );
        assert!(
            csp.contains("script-src 'nonce-n'"),
            "nonce 限定ポリシーが破られた: {csp}"
        );
    }

    #[test]
    fn 許可ホスト検証_空白_クォート_ワイルドカード_改行を拒否する() {
        for bad in [
            "https://evil.com 'unsafe-inline'",
            "https://evil.com\nscript-src *",
            "https://*",
            "https://e\"vil.com",
            "https://e'vil.com",
        ] {
            let allow = ExternalResourceAllow {
                hosts: vec![bad.to_string()],
            };
            assert!(
                validate_allow_hosts(&allow).is_err(),
                "危険なホスト `{bad}` が受理された"
            );
        }
    }

    #[test]
    fn 許可ホスト検証_非_https_スキームを拒否する() {
        for bad in [
            "http://example.com",
            "javascript:alert(1)",
            "data:text/html",
            "example.com",
            "ftp://example.com",
        ] {
            let allow = ExternalResourceAllow {
                hosts: vec![bad.to_string()],
            };
            assert!(
                validate_allow_hosts(&allow).is_err(),
                "非 https スキーム `{bad}` が受理された"
            );
        }
    }

    #[test]
    fn 許可ホスト検証_パスやクエリ付きを拒否する() {
        for bad in [
            "https://example.com/path",
            "https://example.com?q=1",
            "https://example.com#frag",
        ] {
            let allow = ExternalResourceAllow {
                hosts: vec![bad.to_string()],
            };
            assert!(
                validate_allow_hosts(&allow).is_err(),
                "パス/クエリ付き `{bad}` が受理された"
            );
        }
    }

    #[test]
    fn 許可ホスト検証_不正ポートを拒否する() {
        let allow = ExternalResourceAllow {
            hosts: vec!["https://example.com:80x".to_string()],
        };
        assert!(validate_allow_hosts(&allow).is_err());
    }

    #[test]
    fn nonce_は推測困難で毎回異なる() {
        let a = generate_nonce();
        let b = generate_nonce();
        assert_ne!(a, b, "nonce が再利用された");
        assert!(a.len() >= 16, "nonce が短すぎる: {a}");
        // CSP に安全に入る文字（base64url）のみ。
        assert!(
            a.chars()
                .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_'),
            "nonce に不正文字: {a}"
        );
    }
}
