//! 受信パス引数の再正規化・再検証（要件3.2・design doc 9章）。
//!
//! 単一インスタンスでは「先に起動したサーバープロセス」が、後から起動したクライアントが
//! named pipe で転送してきたパスを受け取って開く。**転送パスを信頼してはならない**
//! （同一ユーザーのプロセスとはいえ、コア検証層で必ず再正規化・再検証する＝design doc 9章）。
//!
//! 本モジュールは I/O を行わない純粋ロジック（cargo test の決定論ゲート対象）。
//! 実 FS の存在確認・canonicalize は呼び出し側（`src-tauri`/`pika-cli`）が行い、
//! ここには「文字列としての健全性検査・接頭辞付与・受理/拒否判定」を集約する。
//!
//! 検査内容（要件3.2「UNC/ADS/長パス接頭辞の扱い確定・健全性検査」）:
//! - 絶対パスであること（相対パスは拒否＝転送前にクライアントが絶対化する規約）。
//! - NUL 文字・制御文字を含まない（不正な転送/インジェクション素を弾く）。
//! - ADS（Alternate Data Stream `file.txt:stream`）参照を拒否（既定で開かない＝安全側）。
//! - 長パス（260 文字超）には `\\?\` 接頭辞を付与して扱いを確定する（UNC は `\\?\UNC\`）。

use crate::error::{PikaError, Result};

/// 受信パスの再検証結果（正規化済みパスと、どう判定したかの分岐種別）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VerifiedPath {
    /// 健全性検査を通った正規化済みパス文字列（長パスには接頭辞付与済み）。
    pub normalized: String,
    /// どの種別として受理したか（テストで分岐を観測する）。
    pub kind: PathKind,
}

/// 受理したパスの種別（要件3.2 の「UNC/長パス接頭辞の扱い確定」を観測可能にする）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PathKind {
    /// 通常のドライブ絶対パス（`C:\...`）。
    Drive,
    /// UNC パス（`\\server\share\...`）。
    Unc,
    /// 既に拡張長パス接頭辞付き（`\\?\...`）。
    Extended,
}

/// Windows MAX_PATH（接頭辞無しパスの長さ閾値）。これを超えたら `\\?\` 接頭辞を付与する。
pub const MAX_PATH_NO_PREFIX: usize = 260;

/// 受信したパス引数を再正規化・再検証する。
///
/// クライアントは転送前に絶対パス化する規約（要件3.2）なので、相対パスはここで拒否する
/// （信頼せず＝サーバー側で勝手に cwd 基準に解決しない。それはクライアントの cwd を奪う事故になる）。
///
/// 受理条件:
/// - NUL/制御文字を含まない。
/// - 絶対パス（ドライブ絶対 `X:\`、UNC `\\`、拡張 `\\?\` のいずれか）。
/// - ADS（コロンがドライブレター以外の位置に出る）を含まない。
pub fn verify_received_path(raw: &str) -> Result<VerifiedPath> {
    if raw.is_empty() {
        return Err(PikaError::InvalidArgument("転送パスが空".into()));
    }
    // NUL/制御文字（インジェクション素・転送破損）を弾く。タブ/改行も含めて拒否する。
    if raw.chars().any(|c| c.is_control()) {
        return Err(PikaError::InvalidArgument(
            "転送パスに制御文字が含まれる".into(),
        ));
    }

    let kind = classify_absolute(raw)?;

    // ADS（Alternate Data Stream）検査。ドライブレターの `X:` 以外の位置にコロンがあれば拒否する。
    reject_alternate_data_stream(raw, &kind)?;

    // 長パスへの接頭辞付与（既に拡張接頭辞付きなら何もしない）。
    let normalized = apply_long_path_prefix(raw, &kind);

    Ok(VerifiedPath { normalized, kind })
}

/// 絶対パスか判定し、種別を返す。相対パスはエラー（信頼しない＝再検証で弾く）。
fn classify_absolute(raw: &str) -> Result<PathKind> {
    let bytes = raw.as_bytes();
    // 拡張長パス接頭辞（`\\?\` または `//?/`）。
    if raw.starts_with(r"\\?\") || raw.starts_with("//?/") {
        return Ok(PathKind::Extended);
    }
    // UNC（`\\server\share` または `//server/share`）。`\\?\` は上で処理済み。
    if (raw.starts_with(r"\\") || raw.starts_with("//"))
        && !raw.starts_with(r"\\?")
        && !raw.starts_with("//?")
    {
        return Ok(PathKind::Unc);
    }
    // ドライブ絶対（`X:\` または `X:/`）。`X:rel`（ドライブ相対）は絶対ではないので拒否する。
    if bytes.len() >= 3
        && bytes[0].is_ascii_alphabetic()
        && bytes[1] == b':'
        && (bytes[2] == b'\\' || bytes[2] == b'/')
    {
        return Ok(PathKind::Drive);
    }
    Err(PikaError::InvalidArgument(format!(
        "絶対パスでない転送パスを拒否: {raw}"
    )))
}

/// ADS（Alternate Data Stream）参照を拒否する。
/// ドライブレターの `X:` のコロンのみ許可し、それ以外の位置のコロンは ADS とみなす。
/// 拡張パス（`\\?\X:\...`）は接頭辞直後にドライブレターのコロンが来るのでそれは許可する。
fn reject_alternate_data_stream(raw: &str, kind: &PathKind) -> Result<()> {
    let scan_from = match kind {
        // `X:` のコロン（インデックス1）は許可。それ以降にコロンがあれば ADS。
        PathKind::Drive => 2,
        // UNC はドライブレターのコロンを持たないので最初から走査。
        PathKind::Unc => 0,
        // 拡張パスは `\\?\X:\...`（接頭辞4文字 + `X:`）または `\\?\UNC\...`（ドライブレター無し）。
        // 前者のドライブレターのコロン（インデックス5）まではスキップしてから走査する。
        PathKind::Extended => extended_scan_start(raw),
    };
    if raw.len() > scan_from && raw[scan_from..].contains(':') {
        return Err(PikaError::InvalidArgument(format!(
            "代替データストリーム参照を拒否: {raw}"
        )));
    }
    Ok(())
}

/// 拡張パスで ADS 走査を始めるバイト位置。`\\?\X:` ならドライブレターのコロン直後（6）から、
/// それ以外（`\\?\UNC\...` 等）は接頭辞直後（4）から走査する。
fn extended_scan_start(raw: &str) -> usize {
    let b = raw.as_bytes();
    // `\\?\` は 4 バイト。続けて `X:` ならドライブレターのコロンを許可する。
    if b.len() >= 6 && b[4].is_ascii_alphabetic() && b[5] == b':' {
        6
    } else {
        4
    }
}

/// 長パスに `\\?\` 接頭辞を付与する（既に拡張接頭辞付きなら不変）。
/// UNC は `\\?\UNC\server\share\...` 形式へ畳む。閾値以下のパスは元のまま返す。
fn apply_long_path_prefix(raw: &str, kind: &PathKind) -> String {
    if raw.chars().count() <= MAX_PATH_NO_PREFIX || matches!(kind, PathKind::Extended) {
        return raw.to_string();
    }
    // バックスラッシュへ寄せて接頭辞を付与する（拡張長パスは `/` を区切りに使えないため）。
    let backslashed = raw.replace('/', r"\");
    match kind {
        PathKind::Unc => {
            // `\\server\share\...` → `\\?\UNC\server\share\...`
            let stripped = backslashed.trim_start_matches('\\');
            format!(r"\\?\UNC\{stripped}")
        }
        PathKind::Drive => format!(r"\\?\{backslashed}"),
        PathKind::Extended => backslashed,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ドライブ絶対パスを受理する() {
        let v = verify_received_path(r"C:\dir\a.md").unwrap();
        assert_eq!(v.kind, PathKind::Drive);
        assert_eq!(v.normalized, r"C:\dir\a.md");
    }

    #[test]
    fn unc_パスを受理する() {
        let v = verify_received_path(r"\\server\share\note.md").unwrap();
        assert_eq!(v.kind, PathKind::Unc);
    }

    #[test]
    fn 拡張長パス接頭辞付きを受理する() {
        let v = verify_received_path(r"\\?\C:\very\long\path.md").unwrap();
        assert_eq!(v.kind, PathKind::Extended);
        // 既に接頭辞付きは不変。
        assert_eq!(v.normalized, r"\\?\C:\very\long\path.md");
    }

    #[test]
    fn 相対パスは拒否する() {
        // 信頼しない＝サーバー側で cwd 基準に解決せず弾く（クライアントが絶対化する規約）。
        assert!(verify_received_path(r"dir\a.md").is_err());
        assert!(verify_received_path("a.md").is_err());
    }

    #[test]
    fn ドライブ相対パスは絶対でないので拒否する() {
        // `C:rel`（ドライブ相対）は絶対パスではない。
        assert!(verify_received_path(r"C:rel\a.md").is_err());
    }

    #[test]
    fn nul_文字を含むパスは拒否する() {
        assert!(verify_received_path("C:\\a\0b.md").is_err());
    }

    #[test]
    fn 制御文字を含むパスは拒否する() {
        assert!(verify_received_path("C:\\a\nb.md").is_err());
        assert!(verify_received_path("C:\\a\tb.md").is_err());
    }

    #[test]
    fn ads_参照を拒否する() {
        // ドライブレター以外の位置のコロン＝代替データストリーム。
        assert!(verify_received_path(r"C:\a.md:hidden").is_err());
        assert!(verify_received_path(r"C:\a.md:$DATA").is_err());
    }

    #[test]
    fn unc_でのコロンは_ads_として拒否する() {
        assert!(verify_received_path(r"\\server\share\a.md:stream").is_err());
    }

    #[test]
    fn 長パスには拡張接頭辞を付与する() {
        let long = format!(r"C:\{}\a.md", "x".repeat(300));
        let v = verify_received_path(&long).unwrap();
        assert!(v.normalized.starts_with(r"\\?\C:\"));
    }

    #[test]
    fn 長い_unc_は_unc_接頭辞へ畳む() {
        let long = format!(r"\\server\share\{}\a.md", "y".repeat(300));
        let v = verify_received_path(&long).unwrap();
        assert!(v.normalized.starts_with(r"\\?\UNC\server\share\"));
    }

    #[test]
    fn 空パスは拒否する() {
        assert!(verify_received_path("").is_err());
    }
}
