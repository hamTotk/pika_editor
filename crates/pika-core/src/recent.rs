//! 最近使った項目（ファイル/フォルダ）の純粋ロジック（要件10.2・design doc 9章/19章）。
//!
//! state.json に最近使ったファイル/フォルダを各最大 [`RECENT_CAP`] 件保持する。
//! Windows タスクバーのジャンプリスト表示（`ICustomDestinationList` 等 COM）は実機・実描画が
//! 要るため src-tauri 側の薄い配線（系統C 検証）に置き、**並び順・重複排除・上限切り詰め**の
//! 決定論ロジックだけを本モジュールに集約して cargo test で固める（design doc 3章）。
//!
//! 並び規則: 最近使ったものを先頭に置く LRU。同一パス（大文字小文字無視＝Windows 流儀）を
//! 再オープンしたら既存を取り除いて先頭へ詰め直す（重複を増やさない）。上限超過は末尾を捨てる。

use serde::{Deserialize, Serialize};

/// 最近使った項目の保持上限（ファイル/フォルダそれぞれ・要件10.2「各20件」）。
pub const RECENT_CAP: usize = 20;

/// 最近使った項目リスト（LRU・先頭が最新）。state.json にそのまま直列化する。
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct RecentList {
    /// 最近使ったファイルの絶対パス（先頭が最新・最大 [`RECENT_CAP`] 件）。
    #[serde(default)]
    pub files: Vec<String>,
    /// 最近使ったフォルダの絶対パス（先頭が最新・最大 [`RECENT_CAP`] 件）。
    #[serde(default)]
    pub folders: Vec<String>,
}

impl RecentList {
    /// ファイルを最近使った先頭へ詰める（重複排除＋上限切り詰め）。
    pub fn push_file(&mut self, path: &str) {
        push_recent(&mut self.files, path);
    }

    /// フォルダを最近使った先頭へ詰める（重複排除＋上限切り詰め）。
    pub fn push_folder(&mut self, path: &str) {
        push_recent(&mut self.folders, path);
    }
}

/// LRU 更新の共通処理: 既存の同一パス（大文字小文字無視）を除去し先頭へ、上限超過は末尾切り捨て。
///
/// 空/空白のみのパスは無視する（state.json にゴミを溜めない）。元の表記（大文字小文字）は
/// **新規挿入する `path` の表記を採用**する（最後に開いた表記を残す＝最新を尊重）。
fn push_recent(list: &mut Vec<String>, path: &str) {
    let path = path.trim();
    if path.is_empty() {
        return;
    }
    // 既存の同一パス（Windows なので大文字小文字無視で比較）を取り除く。
    list.retain(|existing| !same_path(existing, path));
    list.insert(0, path.to_string());
    if list.len() > RECENT_CAP {
        list.truncate(RECENT_CAP);
    }
}

/// Windows のパス同一判定（大文字小文字無視・区切りは正規化しない＝呼び出し側が絶対パス正規化済み）。
fn same_path(a: &str, b: &str) -> bool {
    a.eq_ignore_ascii_case(b)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 最新が先頭に来る() {
        let mut r = RecentList::default();
        r.push_file(r"C:\a.md");
        r.push_file(r"C:\b.md");
        assert_eq!(
            r.files,
            vec![r"C:\b.md".to_string(), r"C:\a.md".to_string()]
        );
    }

    #[test]
    fn 同一パス再オープンは重複させず先頭へ詰め直す() {
        let mut r = RecentList::default();
        r.push_file(r"C:\a.md");
        r.push_file(r"C:\b.md");
        r.push_file(r"C:\a.md");
        // a は 1 件だけ・先頭に来る。
        assert_eq!(
            r.files,
            vec![r"C:\a.md".to_string(), r"C:\b.md".to_string()]
        );
    }

    #[test]
    fn 大文字小文字違いは同一視する() {
        let mut r = RecentList::default();
        r.push_file(r"C:\Note.md");
        r.push_file(r"c:\note.MD");
        // 1 件のみ・最後に開いた表記を採用する。
        assert_eq!(r.files, vec![r"c:\note.MD".to_string()]);
    }

    #[test]
    fn 上限を超えたら古いものを捨てる() {
        let mut r = RecentList::default();
        for i in 0..(RECENT_CAP + 5) {
            r.push_file(&format!(r"C:\f{i}.md"));
        }
        assert_eq!(r.files.len(), RECENT_CAP);
        // 最新（最後に push した）が先頭。
        assert_eq!(r.files[0], format!(r"C:\f{}.md", RECENT_CAP + 4));
        // 最古（f0..f4）は押し出されている。
        assert!(!r.files.contains(&r"C:\f0.md".to_string()));
    }

    #[test]
    fn 空や空白パスは無視する() {
        let mut r = RecentList::default();
        r.push_file("");
        r.push_file("   ");
        r.push_folder("");
        assert!(r.files.is_empty());
        assert!(r.folders.is_empty());
    }

    #[test]
    fn ファイルとフォルダは別管理() {
        let mut r = RecentList::default();
        r.push_file(r"C:\a.md");
        r.push_folder(r"C:\ws");
        assert_eq!(r.files, vec![r"C:\a.md".to_string()]);
        assert_eq!(r.folders, vec![r"C:\ws".to_string()]);
    }
}
