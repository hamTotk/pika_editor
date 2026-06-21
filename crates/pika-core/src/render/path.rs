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
    let raw = reference
        .split(['?', '#'])
        .next()
        .unwrap_or("")
        .trim();
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
}
