//! アトミック書込のプリミティブ一本化（最上位原則「データを失わない」・要件12.1）。
//!
//! 役割（design doc 3章「薄い橋渡し層」）: src-tauri 各所の「一時ファイルへ書く→fsync→元パスへ
//! 単一アトミック置換」を **1関数**（[`atomic_write`]）へ集約する。旧実装は3系統あった:
//! - `document::replace_atomically` … `MoveFileExW(MOVEFILE_REPLACE_EXISTING|WRITE_THROUGH)`（最安全）
//! - `state_store::save` … `std::fs::rename`（同一ボリューム置換）
//! - `snapshot_persist::atomic_write` … `std::fs::rename`
//!
//! 後 2 者を最安全の **`MoveFileExW` 単一呼び出し**へ格上げして統一する（消失窓を作らない＝
//! データ損失防止の強化方向）。tmp 名は PID＋連番で一意化し（[`TMP_SEQ`]）、同一 PID 内・
//! 同居プロセスの並行書込で tmp が衝突して相互上書きするのを構造的に防ぐ（#21）。

use std::io;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

/// 一時ファイル名の連番カウンタ（同一 PID 内の並行書込で tmp 名を衝突させない・#21）。
static TMP_SEQ: AtomicU64 = AtomicU64::new(0);

/// 一時ファイルへ書き、fsync 後に元パスへ単一アトミック操作で置換する（要件12.1・最上位原則1）。
///
/// 1. `target` と**同一ディレクトリ**（＝同一ボリューム。rename 置換が成立する条件）に PID＋連番の
///    一意な一時ファイルを作り、全バイトを書いて `sync_all`（fsync）でディスクへ落とす。
/// 2. [`replace_atomically`] で元パスへ**単一の置換 API**で差し替える（途中クラッシュ/電源断でも
///    元ファイルが半端にならない＝最上位原則「データを失わない」）。
///
/// 親ディレクトリが無ければ作る（スナップショット永続化の初回などで成立させる）。一時ファイルの
/// 書込・置換いずれの失敗でも一時ファイルを必ず後始末し、**元ファイルは一切触らない**（消さない＝
/// 半端な状態を作らない）。
pub fn atomic_write(target: &Path, bytes: &[u8]) -> io::Result<()> {
    use std::io::Write;
    // 親が無ければ作る（空＝カレント直下指定のときは作らない）。
    if let Some(parent) = target.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent)?;
        }
    }
    let tmp = tmp_path_for(target);
    // 一時ファイルへ書いて fsync。失敗したら一時ファイルを後始末して返す。
    let write_res = (|| -> io::Result<()> {
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()?;
        Ok(())
    })();
    if let Err(e) = write_res {
        let _ = std::fs::remove_file(&tmp);
        return Err(e);
    }
    // 元パスへ単一アトミック操作で置換。失敗したら一時ファイルを後始末し元は触らない。
    if let Err(e) = replace_atomically(&tmp, target) {
        let _ = std::fs::remove_file(&tmp);
        return Err(e);
    }
    Ok(())
}

/// 元パスと**同一ディレクトリ**の一意な一時ファイルパスを作る（dotfile 隠し・PID＋連番で衝突回避）。
///
/// 同一ディレクトリに置くのは、別ディレクトリ/別ボリュームだと rename がアトミック置換にならない
/// ため。dotfile プレフィックスでツリー上の目立ちを抑える（成功時は置換で即消える短命ファイル）。
fn tmp_path_for(target: &Path) -> PathBuf {
    let dir = match target.parent() {
        Some(p) if !p.as_os_str().is_empty() => p,
        _ => Path::new("."),
    };
    let file_name = target
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| "pika".into());
    let seq = TMP_SEQ.fetch_add(1, Ordering::Relaxed);
    dir.join(format!(
        ".{file_name}.{}.{seq}.pika.tmp",
        std::process::id()
    ))
}

/// 一時ファイルを元パスへ単一アトミック操作で置換する（最上位原則「データを失わない」）。
///
/// Windows: `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` 1 呼び出しで既存を
/// 置換する（remove+rename の 2 段にしない＝間でクラッシュすると対象パスが消える窓を作らない）。
/// 元が存在しない新規作成も同 API で成立する（`MOVEFILE_REPLACE_EXISTING` は対象不在でもエラーに
/// しない）。非 Windows: `std::fs::rename`（POSIX rename は同一ボリュームでアトミック置換）。
#[cfg(windows)]
fn replace_atomically(tmp: &Path, target: &Path) -> io::Result<()> {
    use windows_sys::Win32::Storage::FileSystem::{
        MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH,
    };
    let from = crate::util::to_wide(tmp);
    let to = crate::util::to_wide(target);
    // SAFETY: from/to はヌル終端の有効な UTF-16 パス。MoveFileExW は同期 API で所有権を移さない。
    let ok = unsafe {
        MoveFileExW(
            from.as_ptr(),
            to.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if ok == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

#[cfg(not(windows))]
fn replace_atomically(tmp: &Path, target: &Path) -> io::Result<()> {
    std::fs::rename(tmp, target)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// テスト専用カウンタ（並行実行でも作業ディレクトリ名を衝突させない）。
    static TEST_SEQ: AtomicU64 = AtomicU64::new(0);

    /// 一意な作業ディレクトリを作る（PID＋連番）。後始末は各テストの末尾で行う。
    fn unique_dir() -> PathBuf {
        let mut dir = std::env::temp_dir();
        let seq = TEST_SEQ.fetch_add(1, Ordering::Relaxed);
        dir.push(format!("pika_fs_atomic_test_{}_{seq}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn atomic_write_書込んだ内容を読み戻せる() {
        let dir = unique_dir();
        let target = dir.join("a.txt");
        atomic_write(&target, b"hello").unwrap();
        assert_eq!(std::fs::read(&target).unwrap(), b"hello");
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn atomic_write_既存ファイルを置換する() {
        let dir = unique_dir();
        let target = dir.join("b.txt");
        std::fs::write(&target, b"old-content-longer").unwrap();
        atomic_write(&target, b"new").unwrap();
        // 既存内容を残さず置換する（消失窓を作らず最後に丸ごと差し替わる）。
        assert_eq!(std::fs::read(&target).unwrap(), b"new");
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn atomic_write_対象不在でも新規作成できる() {
        let dir = unique_dir();
        let target = dir.join("c.txt");
        assert!(!target.exists());
        atomic_write(&target, b"created").unwrap();
        assert_eq!(std::fs::read(&target).unwrap(), b"created");
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn atomic_write_親ディレクトリを自動作成する() {
        let dir = unique_dir();
        let target = dir.join("nested").join("deep").join("d.txt");
        assert!(!target.parent().unwrap().exists());
        atomic_write(&target, b"deep").unwrap();
        assert_eq!(std::fs::read(&target).unwrap(), b"deep");
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn atomic_write_成功時に一時ファイルを残さない() {
        let dir = unique_dir();
        let target = dir.join("e.txt");
        atomic_write(&target, b"clean").unwrap();
        // tmp（.<name>.<pid>.<seq>.pika.tmp）が成功後に残っていないこと。
        let leftovers: Vec<String> = std::fs::read_dir(&dir)
            .unwrap()
            .flatten()
            .map(|ent| ent.file_name().to_string_lossy().to_string())
            .filter(|name| name.ends_with(".pika.tmp"))
            .collect();
        assert!(leftovers.is_empty(), "tmp が残存: {leftovers:?}");
        std::fs::remove_dir_all(&dir).ok();
    }
}
