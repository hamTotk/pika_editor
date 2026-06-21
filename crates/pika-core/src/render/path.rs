//! プレビュー custom protocol のローカル参照パス封じ込め（要件6.2/6.3/9.1・design doc 6章）。
//!
//! 未信頼文書（Markdown/HTML）内のローカル相対参照（画像/CSS）を配信する際、基準ディレクトリ
//! （文書のあるフォルダ）から脱出する参照を拒否する。攻撃面は「`../` 連鎖」「絶対パス指定」
//! 「シンボリックリンク経由の脱出」「機密ファイル（`.env`/`*.key` 等）の窃取」。
//!
//! 設計（design doc 6章「基準ディレクトリへ canonicalize＋prefix 検証」）:
//! - 純粋ロジック（[`resolve_local_ref`]）: 要求 URL を相対パスへ正規化し、絶対パス/UNC/ドライブ指定/
//!   `..` 脱出/機密ファイルを**正規化前に**弾く（FS 非依存・cargo test 対象）。
//! - FS 検証（[`confine_under`]）: canonicalize 済みの「基準ディレクトリ」と「解決先」を受け、
//!   解決先が基準配下に収まる（prefix 一致）ことを確認する。**呼び出し側が canonicalize し**、その結果を
//!   本関数へ渡す（シンボリックリンク脱出は canonicalize がリンクを実体へ展開するため prefix 検証で弾ける）。
//!
//! 本モジュールは Tauri/wry を一切知らない純粋ロジック（cargo test の決定論ゲート対象）。

use crate::snapshot::policy::is_sensitive;
use std::path::{Component, Path, PathBuf};

/// ローカル参照の解決結果（要件6.2/6.3）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LocalRefDecision {
    /// 基準ディレクトリからの相対パスとして安全（この相対パスを基準へ結合し FS 検証へ進む）。
    Relative(PathBuf),
    /// 拒否（理由付き）。配信せず壊れた参照のプレースホルダ/エラーマークにする（要件6.2）。
    Reject(RejectReason),
}

/// ローカル参照を拒否する理由（要件6.2/6.3/9.1）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RejectReason {
    /// 絶対パス/ドライブ指定/UNC（基準ディレクトリ外を直接指す）。
    Absolute,
    /// `..` による基準ディレクトリ脱出。
    ParentEscape,
    /// 機密ファイル（`.env`/`*.key`/`*.pem`/`*secret*` 等）への参照（custom protocol からも配信拒否）。
    Sensitive,
    /// 空/不正な参照。
    Empty,
}

/// custom protocol が受けたローカル参照 URL を、基準ディレクトリ相対パスへ正規化判定する（FS 非依存）。
///
/// 受理するのは「基準ディレクトリ配下を指す相対参照」のみ:
/// - 先頭が `/`・`\\`（UNC）・`C:` 等のドライブ指定 → [`RejectReason::Absolute`]
/// - `..` で基準を出る → [`RejectReason::ParentEscape`]
/// - 機密ファイル（ファイル名で判定）→ [`RejectReason::Sensitive`]
/// - 空 → [`RejectReason::Empty`]
///
/// 受理時は `.`/重複区切りを畳んだ相対 [`PathBuf`] を返す。呼び出し側はこれを基準へ結合し
/// canonicalize したうえで [`confine_under`] で prefix 検証する（シンボリックリンク脱出はそこで弾く）。
pub fn resolve_local_ref(reference: &str) -> LocalRefDecision {
    // クエリ/フラグメントを剥がす（custom protocol は ?query を持ちうる）。
    let raw = reference.split(['?', '#']).next().unwrap_or("").trim();
    if raw.is_empty() {
        return LocalRefDecision::Reject(RejectReason::Empty);
    }

    // パーセントデコード（最小限）。エンコードされた `..` や `/` での回避を防ぐ。
    let decoded = percent_decode(raw);

    // バックスラッシュも区切りとして扱う（Windows 文書）。
    let normalized = decoded.replace('\\', "/");

    // 絶対パス/ドライブ/UNC の拒否。
    if normalized.starts_with('/') || normalized.starts_with("//") {
        return LocalRefDecision::Reject(RejectReason::Absolute);
    }
    // `C:` や `C:/...` のドライブ指定。
    let bytes = normalized.as_bytes();
    if bytes.len() >= 2 && bytes[1] == b':' && bytes[0].is_ascii_alphabetic() {
        return LocalRefDecision::Reject(RejectReason::Absolute);
    }

    // 構造的拒否（絶対/`..` 脱出）を先に判定する。`..` 脱出はパス構造の問題であり、
    // 機密ファイル名判定（内容ベース）より優先して報告する（脱出の方が攻撃として重い）。
    // 相対パスのコンポーネントを畳み、`..` の基準脱出を検出する。
    let mut depth: i32 = 0;
    let mut parts: Vec<&str> = Vec::new();
    for seg in normalized.split('/') {
        match seg {
            "" | "." => continue,
            ".." => {
                depth -= 1;
                if depth < 0 {
                    return LocalRefDecision::Reject(RejectReason::ParentEscape);
                }
                parts.pop();
            }
            other => {
                depth += 1;
                parts.push(other);
            }
        }
    }
    if parts.is_empty() {
        return LocalRefDecision::Reject(RejectReason::Empty);
    }

    // 機密ファイル判定（ファイル名部分。custom protocol からも配信拒否＝要件9.1）。
    // 構造的に安全（基準配下に収まる相対パス）でも、機密ファイルは配信しない。
    let relative: PathBuf = parts.iter().collect();
    if is_sensitive(&normalized) {
        return LocalRefDecision::Reject(RejectReason::Sensitive);
    }

    LocalRefDecision::Relative(relative)
}

/// canonicalize 済みの解決先が基準ディレクトリ配下に収まるか検証する（design doc 6章 prefix 検証）。
///
/// `base` と `resolved` は**いずれも呼び出し側が canonicalize 済み**であること（シンボリックリンクは
/// 実体へ展開される＝リンク経由の脱出を prefix 検証で弾ける）。`resolved == base` 配下（または同一）なら
/// `true`。`base` の外を指すなら `false`（脱出＝配信拒否）。
pub fn confine_under(base: &Path, resolved: &Path) -> bool {
    resolved.starts_with(base)
}

/// 相対パス [`PathBuf`] を基準ディレクトリへ結合する（呼び出し側が canonicalize する前段）。
///
/// [`resolve_local_ref`] が返した安全な相対パスを基準と結合するだけ（`..` は既に畳まれている）。
pub fn join_under(base: &Path, relative: &Path) -> PathBuf {
    // relative は resolve_local_ref が `..` を畳んだ通常成分のみ（防御的に Normal のみ採用）。
    let mut out = base.to_path_buf();
    for comp in relative.components() {
        if let Component::Normal(seg) = comp {
            out.push(seg);
        }
    }
    out
}

/// サニタイズ済み body 内の `<img src>` の**相対ローカル参照のみ**を配信ルート（`/local/<gen>/`）へ前置する。
///
/// 背景（実機検証で特定）: comrak→ammonia 後の相対画像は `<img src="img/sample.png">`（相対 URL のまま）。
/// 別WebView の文書 URL は `http://pika-preview.localhost/doc/<gen>` のため、ブラウザは相対 `img/sample.png` を
/// `/doc/img/sample.png` に解決してしまい、custom protocol のローカル配信ルート `/local/<gen>/<相対パス>`
/// （[`super::super`] 側 `local_resource_response`＝[`resolve_local_ref`]）に届かず 404 になる。これを防ぐため、
/// 本関数で相対 `src` を `/local/<gen>/` 配下へ書き換え、ブラウザが配信ルートへ到達できるようにする。
///
/// 不変条件（セキュリティを増減させない）:
/// - **相対参照に `local_prefix` を前置するだけ**（`..` 展開も絶対化もしない）。封じ込め検証は従来どおり
///   配信時の [`resolve_local_ref`]＋canonicalize+prefix 検証（`local_resource_response`）が担う。
/// - **`<img>` の `src` のみ**を対象にする。`<a href>`（.md 等へのアプリ内ナビゲーション）は書き換えない。
/// - **書き換えない src**（絶対/スキーム付き/アンカー）: `http://`・`https://`・`pika-preview:`・`data:`・
///   `mailto:`・`tel:`・`//`（プロトコル相対）・`/`（既にルート絶対）・`#`（フラグメント）で始まるもの。
///   これらはそのまま残す。
///
/// 実装は手書きスキャナ（regex 依存を増やさない・ammonia 出力前提の決定論動作）。ammonia は属性値を `"` で
/// クォートしタグ名/属性名を小文字化するが、防御的に大文字 `<IMG`/`SRC=` も拾う（ASCII 大小無視で照合）。
pub fn rewrite_local_image_refs(html: &str, local_prefix: &str) -> String {
    let bytes = html.as_bytes();
    let mut out = String::with_capacity(html.len() + 16);
    let mut i = 0;

    while i < bytes.len() {
        // `<img` 開始タグを ASCII 大小無視で探す（次が境界＝英数字でないこと）。
        if bytes[i] == b'<'
            && starts_with_ci(&bytes[i..], b"<img")
            && is_tag_name_boundary(bytes, i + 4)
        {
            // タグ全体（次の `>` まで）を切り出す。`>` が無ければ末尾まで（不正 HTML への安全側挙動）。
            let tag_end = find_byte(bytes, i, b'>').unwrap_or(bytes.len());
            let tag = &html[i..tag_end];
            out.push_str(&rewrite_img_tag(tag, local_prefix));
            i = tag_end;
        } else {
            // 非対象バイトはそのまま透過（UTF-8 連続バイトも 1 バイトずつ push して問題ない＝
            // push(char) ではなく元スライスのバイト境界で出力するため、ここは ASCII 1 バイトのみ）。
            // 安全のため char 単位で進める。
            let ch_len = utf8_char_len(bytes[i]);
            out.push_str(&html[i..i + ch_len]);
            i += ch_len;
        }
    }
    out
}

/// 単一の `<img ...>` タグ文字列の `src="..."` 値が相対なら `local_prefix` を前置して返す。
///
/// `src=` を ASCII 大小無視で 1 つ探し、ダブルクォート値（ammonia 出力前提）だけを対象にする。
/// 値が相対参照のときのみ前置詞を足し、それ以外（絶対/スキーム付き/アンカー/見つからない）は無改変で返す。
fn rewrite_img_tag(tag: &str, local_prefix: &str) -> String {
    let bytes = tag.as_bytes();
    // `src=` を探す（属性名境界: 直前が英数字でない＝`srcset` の `src` を誤検出しないため）。
    let mut k = 0;
    while k + 4 <= bytes.len() {
        if starts_with_ci(&bytes[k..], b"src=") && is_attr_name_start(bytes, k) {
            let val_start = k + 4;
            // 値はダブルクォート（ammonia 出力規約）。クォート以外（裸値/単一クォート）は対象外で無改変。
            if val_start < bytes.len() && bytes[val_start] == b'"' {
                let q = val_start + 1;
                if let Some(close) = find_byte(bytes, q, b'"') {
                    let value = &tag[q..close];
                    if is_relative_local_ref(value) {
                        let mut out = String::with_capacity(tag.len() + local_prefix.len());
                        out.push_str(&tag[..q]);
                        out.push_str(local_prefix);
                        out.push_str(value);
                        out.push_str(&tag[close..]);
                        return out;
                    }
                }
            }
            // src= は見つかったが対象外（クォート不一致/絶対/スキーム付き等）。無改変で返す。
            return tag.to_string();
        }
        k += 1;
    }
    tag.to_string()
}

/// `src` の値が「配信ルートへ前置すべき相対ローカル参照」かを判定する。
///
/// 書き換えない（false）もの: 絶対/スキーム付き/プロトコル相対/ルート絶対/フラグメント。
/// それ以外（`img/x.png`・`./a.png`・`a/b.png` 等）は相対とみなし true。
fn is_relative_local_ref(value: &str) -> bool {
    let v = value.trim();
    if v.is_empty() {
        return false;
    }
    // ルート絶対・プロトコル相対・フラグメント。
    if v.starts_with('/') || v.starts_with('#') {
        return false;
    }
    // スキーム付き（`scheme:` の存在）。`:` が最初の `/` より前にあればスキームとみなす
    // （`http:`・`https:`・`data:`・`mailto:`・`tel:`・`pika-preview:` 等を一括で除外）。
    if let Some(colon) = v.find(':') {
        let slash = v.find('/').unwrap_or(usize::MAX);
        if colon < slash {
            // スキーム部が RFC 的に妥当（英字始まり・英数/+-. のみ）なら絶対 URL とみなして除外。
            let scheme = &v[..colon];
            if is_uri_scheme(scheme) {
                return false;
            }
        }
    }
    true
}

/// 文字列が URI スキーム（`ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )`）として妥当か。
fn is_uri_scheme(s: &str) -> bool {
    let mut chars = s.chars();
    match chars.next() {
        Some(c) if c.is_ascii_alphabetic() => {}
        _ => return false,
    }
    chars.all(|c| c.is_ascii_alphanumeric() || c == '+' || c == '-' || c == '.')
}

/// `haystack` が `needle`（ASCII 小文字想定）で大小無視に始まるか。
fn starts_with_ci(haystack: &[u8], needle: &[u8]) -> bool {
    if haystack.len() < needle.len() {
        return false;
    }
    haystack[..needle.len()]
        .iter()
        .zip(needle)
        .all(|(h, n)| h.eq_ignore_ascii_case(n))
}

/// `<img` の直後（位置 `pos`）がタグ名境界か（英数字でない＝`<imgx>` のような別タグを誤検出しない）。
fn is_tag_name_boundary(bytes: &[u8], pos: usize) -> bool {
    match bytes.get(pos) {
        None => true,
        Some(b) => !b.is_ascii_alphanumeric(),
    }
}

/// `src=` の直前が属性名の開始境界か（直前が英数字でない＝`srcset`/`xsrc=` の誤検出を防ぐ）。
fn is_attr_name_start(tag: &[u8], pos: usize) -> bool {
    if pos == 0 {
        return true;
    }
    !tag[pos - 1].is_ascii_alphanumeric()
}

/// `bytes` の `from` 以降で最初の `target` の位置を返す。
fn find_byte(bytes: &[u8], from: usize, target: u8) -> Option<usize> {
    (from..bytes.len()).find(|&j| bytes[j] == target)
}

/// UTF-8 先頭バイトから文字のバイト長を返す（透過コピーをバイト境界で進めるため）。
fn utf8_char_len(b: u8) -> usize {
    if b < 0x80 {
        1
    } else if b >> 5 == 0b110 {
        2
    } else if b >> 4 == 0b1110 {
        3
    } else if b >> 3 == 0b11110 {
        4
    } else {
        // 不正な継続/先頭バイトは 1 バイト進める（壊れた入力への安全側挙動）。
        1
    }
}

/// 最小限のパーセントデコード（`%2e`/`%2f` 等での `..`/`/` 回避を防ぐ）。
fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let (Some(h), Some(l)) = (hex_val(bytes[i + 1]), hex_val(bytes[i + 2])) {
                out.push((h << 4) | l);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn hex_val(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(b - b'a' + 10),
        b'A'..=b'F' => Some(b - b'A' + 10),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 通常の相対参照は受理する() {
        match resolve_local_ref("images/diagram.png") {
            LocalRefDecision::Relative(p) => assert_eq!(p, PathBuf::from("images/diagram.png")),
            other => panic!("受理されなかった: {other:?}"),
        }
    }

    #[test]
    fn 親ディレクトリ脱出を拒否する() {
        assert_eq!(
            resolve_local_ref("../../secret.txt"),
            LocalRefDecision::Reject(RejectReason::ParentEscape)
        );
        // 途中で潜って出るのも、最終的に基準を出れば拒否。
        assert_eq!(
            resolve_local_ref("a/../../b"),
            LocalRefDecision::Reject(RejectReason::ParentEscape)
        );
    }

    #[test]
    fn 一旦潜ってから戻る相対は受理する() {
        match resolve_local_ref("a/b/../c.png") {
            LocalRefDecision::Relative(p) => assert_eq!(p, PathBuf::from("a/c.png")),
            other => panic!("受理されなかった: {other:?}"),
        }
    }

    #[test]
    fn 絶対パスとドライブ指定とunc_を拒否する() {
        assert_eq!(
            resolve_local_ref("/etc/passwd"),
            LocalRefDecision::Reject(RejectReason::Absolute)
        );
        assert_eq!(
            resolve_local_ref("C:/Windows/system32"),
            LocalRefDecision::Reject(RejectReason::Absolute)
        );
        assert_eq!(
            resolve_local_ref(r"\\server\share\x"),
            LocalRefDecision::Reject(RejectReason::Absolute)
        );
        assert_eq!(
            resolve_local_ref("//server/share"),
            LocalRefDecision::Reject(RejectReason::Absolute)
        );
    }

    #[test]
    fn パーセントエンコードでの脱出を拒否する() {
        // %2e%2e%2f = ../
        assert_eq!(
            resolve_local_ref("%2e%2e%2fsecret"),
            LocalRefDecision::Reject(RejectReason::ParentEscape)
        );
        // %2f = / の先頭は絶対扱い。
        assert_eq!(
            resolve_local_ref("%2fetc%2fpasswd"),
            LocalRefDecision::Reject(RejectReason::Absolute)
        );
    }

    #[test]
    fn 機密ファイルへの参照を拒否する() {
        assert_eq!(
            resolve_local_ref(".env"),
            LocalRefDecision::Reject(RejectReason::Sensitive)
        );
        assert_eq!(
            resolve_local_ref("config/server.key"),
            LocalRefDecision::Reject(RejectReason::Sensitive)
        );
        assert_eq!(
            resolve_local_ref("certs/app.pem"),
            LocalRefDecision::Reject(RejectReason::Sensitive)
        );
        assert_eq!(
            resolve_local_ref("notes/my-secret.txt"),
            LocalRefDecision::Reject(RejectReason::Sensitive)
        );
    }

    #[test]
    fn 空参照を拒否する() {
        assert_eq!(
            resolve_local_ref(""),
            LocalRefDecision::Reject(RejectReason::Empty)
        );
        assert_eq!(
            resolve_local_ref("?x=1"),
            LocalRefDecision::Reject(RejectReason::Empty)
        );
        assert_eq!(
            resolve_local_ref("."),
            LocalRefDecision::Reject(RejectReason::Empty)
        );
    }

    #[test]
    fn クエリとフラグメントを剥がす() {
        match resolve_local_ref("img/a.png?v=2#frag") {
            LocalRefDecision::Relative(p) => assert_eq!(p, PathBuf::from("img/a.png")),
            other => panic!("受理されなかった: {other:?}"),
        }
    }

    #[test]
    fn prefix検証_配下は受理_外は拒否() {
        let base = Path::new("/base/dir");
        assert!(confine_under(base, Path::new("/base/dir/img/a.png")));
        assert!(confine_under(base, Path::new("/base/dir")));
        // canonicalize がリンクを展開した結果が外を指せば拒否される。
        assert!(!confine_under(base, Path::new("/other/secret")));
        assert!(!confine_under(base, Path::new("/base/dir-sibling/x")));
    }

    #[test]
    fn join_under_は相対を基準へ結合する() {
        let base = Path::new("/base");
        let rel = PathBuf::from("img/a.png");
        assert_eq!(join_under(base, &rel), PathBuf::from("/base/img/a.png"));
    }

    #[test]
    fn rewrite_相対img_srcを配信ルートへ前置する() {
        let html = r#"<p>図</p><img src="img/sample.png" alt="図">"#;
        let out = rewrite_local_image_refs(html, "/local/7/");
        assert_eq!(
            out,
            r#"<p>図</p><img src="/local/7/img/sample.png" alt="図">"#
        );
    }

    #[test]
    fn rewrite_絶対やスキーム付きやアンカーは不変() {
        // http(s)://・pika-preview:・data:・mailto:・tel:・//・/・# は書き換えない。
        for src in [
            "http://example.com/a.png",
            "https://example.com/a.png",
            "pika-preview://localhost/doc/1",
            "data:image/png;base64,iVBOR",
            "//cdn.example.com/a.png",
            "/abs.png",
            "#frag",
        ] {
            let html = format!(r#"<img src="{src}">"#);
            let out = rewrite_local_image_refs(&html, "/local/3/");
            assert_eq!(out, html, "不変であるべき src `{src}` が書き換えられた");
        }
        // mailto:/tel: は画像 src には通常出ないが、スキーム除外規則の回帰防止として確認。
        for src in ["mailto:a@b.example", "tel:+81-3-0000-0000"] {
            let html = format!(r#"<img src="{src}">"#);
            assert_eq!(rewrite_local_image_refs(&html, "/local/3/"), html);
        }
    }

    #[test]
    fn rewrite_a_hrefは書き換えない() {
        // <a href> はアプリ内ナビゲーション（.md 等）であって配信リソースではない。
        let html = r#"<a href="other.md">他の文書</a><img src="pic.png">"#;
        let out = rewrite_local_image_refs(html, "/local/5/");
        assert_eq!(
            out,
            r#"<a href="other.md">他の文書</a><img src="/local/5/pic.png">"#
        );
    }

    #[test]
    fn rewrite_複数imgと属性順序違いでもsrcのみ前置する() {
        // 属性順序違い（alt が先）＋複数 img。src のみ書き換わり alt は不変。
        let html = r#"<img alt="a" src="rel.png"><img src="dir/b.jpg" alt="b"><img src="https://x/c.png">"#;
        let out = rewrite_local_image_refs(html, "/local/2/");
        assert_eq!(
            out,
            r#"<img alt="a" src="/local/2/rel.png"><img src="/local/2/dir/b.jpg" alt="b"><img src="https://x/c.png">"#
        );
    }

    #[test]
    fn rewrite_srcsetは誤検出しない() {
        // 属性名境界判定で `srcset` の `src` を誤検出しないこと（src 属性が無ければ無改変）。
        let html = r#"<img srcset="a.png 1x, b.png 2x">"#;
        assert_eq!(
            rewrite_local_image_refs(html, "/local/9/"),
            html,
            "srcset を src と誤検出した"
        );
    }

    #[test]
    fn rewrite_dot_スラッシュ始まりの相対も前置する() {
        let html = r#"<img src="./img/x.png">"#;
        assert_eq!(
            rewrite_local_image_refs(html, "/local/4/"),
            r#"<img src="/local/4/./img/x.png">"#
        );
    }

    #[test]
    fn rewrite_大文字タグと属性も対象にする() {
        // ammonia は小文字化するが防御的に大文字 <IMG SRC= も拾う。
        let html = r#"<IMG SRC="rel.png">"#;
        assert_eq!(
            rewrite_local_image_refs(html, "/local/1/"),
            r#"<IMG SRC="/local/1/rel.png">"#
        );
    }

    #[test]
    fn rewrite_日本語本文を壊さない() {
        // マルチバイト本文を透過コピーで一字一句保持しつつ img だけ書き換える。
        let html = "本文の日本語テキスト。<img src=\"画像/絵.png\">あと。";
        assert_eq!(
            rewrite_local_image_refs(html, "/local/8/"),
            "本文の日本語テキスト。<img src=\"/local/8/画像/絵.png\">あと。"
        );
    }
}
