//! コマンドのパス検証ゲート（任意ファイル読み書き封じ込め・#5/#46）。
//!
//! 役割（design doc 3章「薄い境界」・最小権限）:
//! - frontend からの invoke で渡るパス文字列は信頼境界の外側なので、I/O を行う command が
//!   実ファイルへ触る前に **「開いているワークスペース配下」または「明示的に許可されたファイル」**
//!   へ封じ込める（任意パスの読み書きを塞ぐ＝最上位原則の防御的入力検証）。
//! - 健全性検査（絶対パス必須・制御文字/ADS/相対拒否・長パス接頭辞付与）は
//!   `pika_core::path_verify::verify_received_path`、prefix 封じ込めは
//!   `pika_core::render::confine_under` に委ねる（純粋ロジックは core・cargo test 済み）。
//! - 本層は「許可集合（root＋個別ファイル）の保持」と「canonicalize による実体解決」のみを担う。
//!
//! 許可の入口は backend の信頼境界 3 つだけ:
//! - `open_workspace`（フォルダ＝root を張る）
//! - `single_instance` の open-request emit 直前（転送パスを個別許可）
//! - `restore_app_state`（復元する workspace を root・各 tab を個別許可）
//!
//! #46（symlink/junction 経由の外部上書き防止）: 書込検証は **親ディレクトリ**を canonicalize して
//! 封じ込め判定する（新規ファイルはファイル自体が無くてよいため）。リンク先が許可域外なら拒否する。

use pika_core::path_verify::verify_received_path;
use pika_core::render::confine_under;
use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

/// パスアクセス制御（managed state）。許可された読み書き対象を保持する。
pub struct AccessControl {
    inner: Mutex<AccessInner>,
}

#[derive(Default)]
struct AccessInner {
    /// canonicalize 済みワークスペースルート（open_workspace / restore で張る）。
    root: Option<PathBuf>,
    /// 個別許可ファイル（canonicalize 済み）。単一ファイルを開く導線（open-request / restore タブ）用。
    allowed_files: HashSet<PathBuf>,
    /// 個別許可ファイルの親ディレクトリ（canonicalize 済み）。新規保存（同フォルダ内）を通すため。
    allowed_dirs: HashSet<PathBuf>,
}

impl AccessControl {
    pub fn new() -> Self {
        Self {
            inner: Mutex::new(AccessInner::default()),
        }
    }

    /// lock 失敗（毒化）を文字列エラーへ畳む共通ヘルパ。
    fn lock(&self) -> Result<std::sync::MutexGuard<'_, AccessInner>, String> {
        self.inner.lock().map_err(|_| "アクセス制御ロック失敗".to_string())
    }

    /// ワークスペースルートを張る（open_workspace / restore_app_state の workspace）。
    ///
    /// canonicalize 成功時のみ更新する（解決できないパスでは root を変えない＝安全側）。
    pub fn set_root(&self, raw: &str) {
        if let Ok(canon) = std::fs::canonicalize(raw) {
            if let Ok(mut inner) = self.inner.lock() {
                inner.root = Some(canon);
            }
        }
    }

    /// 個別ファイルを許可する（open-request の転送パス / restore タブのパス）。
    ///
    /// ベストエフォート登録: canonicalize 成功時のみ allowed_files へ、親も解決できれば allowed_dirs へ。
    /// 失敗は握りつぶす（許可できないパスは後段の verify_* が封じ込めで弾く）。
    pub fn allow_file(&self, raw: &str) {
        let Ok(canon) = std::fs::canonicalize(raw) else {
            return;
        };
        let parent_canon = canon.parent().and_then(|p| std::fs::canonicalize(p).ok());
        if let Ok(mut inner) = self.inner.lock() {
            inner.allowed_files.insert(canon);
            if let Some(parent) = parent_canon {
                inner.allowed_dirs.insert(parent);
            }
        }
    }

    /// 読み取り対象を検証し canonicalize 済み実体パスを返す（#5 封じ込め）。
    ///
    /// 1. `verify_received_path` で健全性検査（絶対パス/制御文字/ADS/相対）。
    /// 2. canonicalize で実体を解決（symlink/junction を展開）。
    /// 3. root 配下 OR 個別許可ファイル OR 個別許可ディレクトリ配下なら許可。
    ///
    /// 機密ファイルの読みはここでは許可する（封じ込めのみ。ベースラインは別途ハッシュのみ方針）。
    pub fn verify_read(&self, raw: &str) -> Result<PathBuf, String> {
        verify_received_path(raw).map_err(|e| e.to_string())?;
        let canon = std::fs::canonicalize(raw).map_err(|e| format!("パス解決に失敗: {e}"))?;
        let inner = self.lock()?;
        if is_allowed_path(&inner, &canon) {
            Ok(canon)
        } else {
            Err("許可されていないパスです".to_string())
        }
    }

    /// 書き込み対象を検証する（#5＋#46 封じ込め）。
    ///
    /// 新規ファイルはファイル自体が存在しなくてよいため、**親ディレクトリ**を canonicalize して
    /// 封じ込め判定する（symlink/junction 経由で許可域外へ書くのを防ぐ＝#46）。
    /// 既存ファイル自体が個別許可されている場合も許可する（リネーム済み等の取りこぼし防止）。
    pub fn verify_write(&self, raw: &str) -> Result<(), String> {
        verify_received_path(raw).map_err(|e| e.to_string())?;
        let parent = Path::new(raw)
            .parent()
            .ok_or_else(|| "親ディレクトリが取れません".to_string())?;
        let cparent =
            std::fs::canonicalize(parent).map_err(|e| format!("親ディレクトリ解決に失敗: {e}"))?;
        let inner = self.lock()?;
        let parent_ok = inner
            .root
            .as_ref()
            .map(|r| confine_under(r, &cparent))
            .unwrap_or(false)
            || inner.allowed_dirs.contains(&cparent);
        // 親が許可域外でも、ファイル自体が個別許可されていれば通す（既存ファイルの上書き保存）。
        let file_ok = std::fs::canonicalize(raw)
            .ok()
            .map(|c| inner.allowed_files.contains(&c))
            .unwrap_or(false);
        if parent_ok || file_ok {
            Ok(())
        } else {
            Err("許可されていないパスです".to_string())
        }
    }
}

impl Default for AccessControl {
    fn default() -> Self {
        Self::new()
    }
}

/// canonicalize 済みパスが許可域内か（root 配下 / 個別ファイル / 個別ディレクトリ配下）。
fn is_allowed_path(inner: &AccessInner, canon: &Path) -> bool {
    if let Some(root) = &inner.root {
        if confine_under(root, canon) {
            return true;
        }
    }
    if inner.allowed_files.contains(canon) {
        return true;
    }
    canon
        .parent()
        .map(|p| inner.allowed_dirs.contains(p))
        .unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    /// 一時ディレクトリを作る（テスト用・衝突回避に nanos＋連番）。
    fn temp_dir(tag: &str) -> PathBuf {
        use std::sync::atomic::{AtomicU64, Ordering};
        static SEQ: AtomicU64 = AtomicU64::new(0);
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        let seq = SEQ.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!("pika-access-{tag}-{nanos}-{seq}"));
        std::fs::create_dir_all(&dir).expect("一時ディレクトリ作成");
        dir
    }

    /// 指定ディレクトリにファイルを書いて絶対パス文字列を返す。
    fn write_file(dir: &Path, name: &str, body: &str) -> String {
        let p = dir.join(name);
        let mut f = std::fs::File::create(&p).expect("ファイル作成");
        f.write_all(body.as_bytes()).expect("書込");
        // canonicalize で実体パス（\\?\ プレフィクス込み）に揃える。
        std::fs::canonicalize(&p)
            .expect("canonicalize")
            .to_string_lossy()
            .to_string()
    }

    #[test]
    fn root配下のファイルは読み取り許可される() {
        let dir = temp_dir("root-ok");
        let file = write_file(&dir, "a.md", "本文");
        let ac = AccessControl::new();
        ac.set_root(&dir.to_string_lossy());
        assert!(ac.verify_read(&file).is_ok(), "root 配下の読みが拒否された");
    }

    #[test]
    fn root外のファイルは読み取り拒否される() {
        let root = temp_dir("root-in");
        let outside = temp_dir("root-out");
        let file = write_file(&outside, "secret.md", "外部");
        let ac = AccessControl::new();
        ac.set_root(&root.to_string_lossy());
        assert!(
            ac.verify_read(&file).is_err(),
            "root 外のファイル読みが許可された（封じ込め破れ）"
        );
    }

    #[test]
    fn allow_fileした単一ファイルは読み取り許可される() {
        let dir = temp_dir("allow-file");
        let file = write_file(&dir, "single.md", "単体");
        let ac = AccessControl::new();
        // root は張らず、個別ファイルのみ許可する（単一ファイルを開く導線）。
        ac.allow_file(&file);
        assert!(
            ac.verify_read(&file).is_ok(),
            "個別許可ファイルの読みが拒否された"
        );
    }

    #[test]
    fn 相対パスは読み取り拒否される() {
        let ac = AccessControl::new();
        // root を張っても、健全性検査（絶対パス必須）で相対パスは弾かれる。
        ac.set_root(&std::env::temp_dir().to_string_lossy());
        assert!(
            ac.verify_read("relative\\path.md").is_err(),
            "相対パスが許可された"
        );
    }

    #[test]
    fn 制御文字を含むパスは読み取り拒否される() {
        let ac = AccessControl::new();
        ac.set_root(&std::env::temp_dir().to_string_lossy());
        assert!(
            ac.verify_read("C:\\tmp\\bad\u{0007}.md").is_err(),
            "制御文字を含むパスが許可された"
        );
    }

    #[test]
    fn root配下への書き込みは許可される() {
        let dir = temp_dir("write-ok");
        let ac = AccessControl::new();
        ac.set_root(&dir.to_string_lossy());
        // まだ存在しない新規ファイルでも、親（root 配下）が解決できれば許可する。
        let new_file = dir.join("new.md").to_string_lossy().to_string();
        assert!(
            ac.verify_write(&new_file).is_ok(),
            "root 配下への新規書込が拒否された"
        );
    }

    #[test]
    fn root外への書き込みは拒否される() {
        let root = temp_dir("write-in");
        let outside = temp_dir("write-out");
        let ac = AccessControl::new();
        ac.set_root(&root.to_string_lossy());
        let new_file = outside.join("evil.md").to_string_lossy().to_string();
        assert!(
            ac.verify_write(&new_file).is_err(),
            "root 外への書込が許可された（封じ込め破れ）"
        );
    }

    #[test]
    fn allow_fileした親ディレクトリへの新規書き込みは許可される() {
        let dir = temp_dir("write-allow");
        let file = write_file(&dir, "existing.md", "既存");
        let ac = AccessControl::new();
        // 単一ファイルを開いた状態（root 未設定）でも、同フォルダ内の新規保存は通す。
        ac.allow_file(&file);
        let sibling = dir.join("sibling.md").to_string_lossy().to_string();
        assert!(
            ac.verify_write(&sibling).is_ok(),
            "個別許可ファイルと同フォルダへの書込が拒否された"
        );
    }
}
