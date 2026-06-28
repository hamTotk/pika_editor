//! pika のアトミック書込が作る一時ファイルの判定（要件7.1・自己保存抑制の補強）。
//!
//! 保存（`src-tauri/src/fs_atomic.rs::atomic_write`）は監視対象のワークスペース内に
//! `.{file_name}.{pid}.{seq}.pika.tmp` という一時ファイルを作り、`MoveFileExW` で元パスへ
//! アトミック置換する。この一時ファイルは pika 自身が一瞬だけ作る内部ファイルであり、
//! watcher が拾って Create/Modify/Remove イベントを撒く・ベースライン台帳へ載せると
//! ツリー/未読が汚れる。本判定で**監視・列挙から完全除外**する（自己保存抑制の前段）。
//!
//! 判定は I/O を行わない純粋なパス文字列判定（cargo test の決定論ゲート対象）。
//! `fs_atomic` の命名規則と一致させ、予約サフィックス `.pika.tmp` で末尾一致判定する。

/// pika の一時ファイル（`*.pika.tmp`）か。ファイル名が予約サフィックス `.pika.tmp` で終わるか判定する。
///
/// `fs_atomic::tmp_path_for` が作る `.{file_name}.{pid}.{seq}.pika.tmp` を捕捉する。パスは `/` でも
/// `\`（Windows）でも区切られうるため、最終成分（ファイル名）だけを取り出して末尾一致を見る。
///
/// 例: `.foo.md.1234.0.pika.tmp` = true / `foo.md` = false / `foo.pika.tmp.md` = false。
pub fn is_pika_temp(path: &str) -> bool {
    let name = path.rsplit(['/', '\\']).next().unwrap_or(path);
    name.ends_with(".pika.tmp")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pika一時ファイルを判定する() {
        // fs_atomic の命名（.{name}.{pid}.{seq}.pika.tmp）を捕捉する。
        assert!(is_pika_temp(".foo.md.1234.0.pika.tmp"));
        assert!(is_pika_temp("a.txt.999.7.pika.tmp"));
        // パス区切り（/ と \）混在でも最終成分で判定する。
        assert!(is_pika_temp("/ws/sub/.note.md.42.1.pika.tmp"));
        assert!(is_pika_temp(r"C:\ws\sub\.note.md.42.1.pika.tmp"));
    }

    #[test]
    fn 通常ファイルは一時ファイルでない() {
        assert!(!is_pika_temp("foo.md"));
        assert!(!is_pika_temp("/ws/foo.md"));
        // サフィックスが末尾でない（途中に .pika.tmp があるだけ）は対象外。
        assert!(!is_pika_temp("foo.pika.tmp.md"));
        // ディレクトリ名に .pika.tmp を含んでもファイル名が通常なら対象外（最終成分で判定）。
        assert!(!is_pika_temp("/ws/x.pika.tmp/real.md"));
        assert!(!is_pika_temp(""));
    }
}
