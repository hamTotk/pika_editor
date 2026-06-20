//! Tauri command 層（薄い境界）。ロジックは pika-core に置き、ここは I/O 配線のみ。
//!
//! 本スプリント（sprint 1・最薄ループ）は中心体験① の貫通の土台として
//! フォルダ列挙/ファイル読込/保存の最小 command を提供する。
//! ツリー列挙の除外/自然順・エンコーディング往復・自己保存抑制は後続スプリントで
//! pika-core（workspace/document/watcher）へ移す（design doc 4章）。

use serde::Serialize;
use std::path::Path;

/// ツリー 1 段分のエントリ（最薄ループ用の最小情報）。
#[derive(Debug, Serialize)]
pub struct TreeEntry {
    pub name: String,
    pub path: String,
    pub is_dir: bool,
}

/// フォルダを開いて直下のエントリ一覧を返す（最薄ループ）。
///
/// sprint 2 で workspace モジュール（除外リスト・自然順・シンボリックリンク循環検出）に置換する。
#[tauri::command]
pub fn open_workspace(path: String) -> Result<Vec<TreeEntry>, String> {
    let dir = Path::new(&path);
    if !dir.is_dir() {
        return Err(format!("フォルダではありません: {path}"));
    }
    let mut entries = Vec::new();
    let read = std::fs::read_dir(dir).map_err(|e| format!("読み取りに失敗: {e}"))?;
    for ent in read.flatten() {
        let p = ent.path();
        let name = ent.file_name().to_string_lossy().to_string();
        let is_dir = p.is_dir();
        entries.push(TreeEntry {
            name,
            path: p.to_string_lossy().to_string(),
            is_dir,
        });
    }
    // 暫定の安定順（自然順は sprint 2 の workspace モジュールで実装）。
    entries.sort_by(|a, b| (b.is_dir, &a.name).cmp(&(a.is_dir, &b.name)));
    Ok(entries)
}

/// ファイル内容を読む（最薄ループ）。エンコーディング判定は sprint 6 の document へ。
#[tauri::command]
pub fn read_file(path: String) -> Result<String, String> {
    std::fs::read_to_string(&path).map_err(|e| format!("読み込みに失敗: {e}"))
}

/// ファイルを保存する（最薄ループ）。アトミック書込・自己保存抑制は後続スプリントへ。
///
/// 本来は Channel API でバイナリ転送するが（design doc 3章 IPC 予算）、最薄ループでは
/// テキスト invoke で配線し、IPC コスト実測（系統C・15章-1）で転送方式を確定する。
#[tauri::command]
pub fn save_file(path: String, content: String) -> Result<(), String> {
    std::fs::write(&path, content).map_err(|e| format!("保存に失敗: {e}"))
}
