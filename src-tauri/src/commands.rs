//! Tauri command 層（薄い境界）。ロジックは pika-core に置き、ここは I/O 配線のみ。
//!
//! 本スプリント（sprint 1・最薄ループ）は中心体験① の貫通の土台として
//! フォルダ列挙/ファイル読込/保存の最小 command を提供する。
//! ツリー列挙の除外/自然順・エンコーディング往復・自己保存抑制は後続スプリントで
//! pika-core（workspace/document/watcher）へ移す（design doc 4章）。

use crate::snapshot::SnapshotService;
use crate::state_store;
use crate::watcher::WatcherService;
use pika_core::state::{AppState, TabRestore, WorkspaceRestore};
use serde::{Deserialize, Serialize};
use std::hash::Hasher;
use std::path::Path;
use tauri::State;

/// ツリー 1 段分のエントリ（最薄ループ用の最小情報）。
#[derive(Debug, Serialize)]
pub struct TreeEntry {
    pub name: String,
    pub path: String,
    pub is_dir: bool,
}

/// フォルダを開いて直下のエントリ一覧を返す。開いたルートの監視も開始する（sprint 2）。
///
/// 監視開始（要件7.1）: open 時にベースラインを取得し、以後の外部変更を
/// `emit('fs-changed')` でフロントへ通知する（自前合成層は pika-core::watcher）。
/// 除外リスト/自然順/シンボリックリンク循環検出は後続スプリントで workspace モジュールへ移す。
#[tauri::command]
pub fn open_workspace(
    path: String,
    watcher: State<'_, WatcherService>,
    snapshot: State<'_, SnapshotService>,
) -> Result<Vec<TreeEntry>, String> {
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
        // 差分のベースライン内容を取得（全既読スタート＝要件8.1）。
        // 機密/10MB以上/画像はハッシュのみ（pika-core::snapshot::policy が判定）。
        if !is_dir {
            capture_baseline_for(&p, &snapshot);
        }
        entries.push(TreeEntry {
            name,
            path: p.to_string_lossy().to_string(),
            is_dir,
        });
    }
    // 暫定の安定順（自然順は後続スプリントの workspace モジュールで実装）。
    entries.sort_by(|a, b| (b.is_dir, &a.name).cmp(&(a.is_dir, &b.name)));

    // 監視を開始（ベースライン取得・外部変更の emit）。監視不能 FS はポーリングへ縮退する。
    watcher.watch_root(dir)?;
    Ok(entries)
}

/// 差分のベースライン内容を取得する（要件8.1）。テキスト読取に失敗（バイナリ等）は
/// サイズだけ渡してハッシュのみベースライン化を pika-core 判定に委ねる。
fn capture_baseline_for(path: &Path, snapshot: &SnapshotService) {
    let path_str = path.to_string_lossy().to_string();
    let size = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    let content = std::fs::read_to_string(path).unwrap_or_default();
    snapshot.capture_baseline(&path_str, &content, size);
}

/// ファイル内容を読む（最薄ループ）。エンコーディング判定は sprint 6 の document へ。
#[tauri::command]
pub fn read_file(path: String) -> Result<String, String> {
    std::fs::read_to_string(&path).map_err(|e| format!("読み込みに失敗: {e}"))
}

/// ファイルを保存する。保存直前に自己保存トークンを登録し、watcher の自己イベントを抑制する。
///
/// 自己保存抑制（要件7.1）: 保存内容を LF 正規化したハッシュをトークンとして登録する。
/// watcher 側は現ディスク内容のハッシュ一致をもってワンショットで抑制する（未読を付けない）。
/// アトミック書込（一時ファイル→置換）は sprint 6 で document へ移す。
#[tauri::command]
pub fn save_file(
    path: String,
    content: String,
    watcher: State<'_, WatcherService>,
) -> Result<(), String> {
    // 保存後ハッシュを先に算出して登録（watcher イベントより先に勝つ必要があるため保存前に登録）。
    let saved_hash = hash_normalized(content.as_bytes());
    watcher.register_self_save(&path, &saved_hash);
    std::fs::write(&path, content).map_err(|e| format!("保存に失敗: {e}"))
}

/// F5（要件7.1/11.2）= オンデマンドの全体再スキャン＋再同期。検知した変更件数を返す。
/// ポーリング/オーバーフロー再同期と同じ pika-core::watcher の処理を共有する。
#[tauri::command]
pub fn f5_resync(watcher: State<'_, WatcherService>) -> Result<usize, String> {
    watcher.resync_now()
}

/// アプリ状態（state.json）をアトミックに保存する（要件10.1・design doc 9章）。
///
/// 直列化・version はすべて pika-core::state（cargo test 済み）。ここはデータルート解決と
/// アトミック書込（一時ファイル→置換）の FS 配線のみ（state_store）。
#[tauri::command]
pub fn save_app_state(state: AppState) -> Result<(), String> {
    let root = state_store::resolve()?;
    state_store::save(&root, &state)
}

/// 復元したタブ 1 件の DTO（フロントが復元3分岐を区別できるよう種別を付ける）。
#[derive(Debug, Serialize, Deserialize)]
pub struct RestoredTabDto {
    /// "restore"（正常）| "deleted"（消失=削除済み表示）| "unread"（別物=未読復元）
    pub status: String,
    pub tab: pika_core::state::TabState,
}

/// 復元結果の DTO（ワークスペース判定＋タブ群＋安全空起動フラグ）。
#[derive(Debug, Serialize, Deserialize)]
pub struct RestoreOutcomeDto {
    /// "restore" | "empty-state"（ワークスペース消失）| "no-workspace"（単体ファイルのみ）
    pub workspace_status: String,
    /// 復元するワークスペースの絶対パス（restore のときのみ）。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub workspace_path: Option<String>,
    pub tabs: Vec<RestoredTabDto>,
    /// 未知バージョン/破損で空起動した（このセッションは上書き保存を控える＝読めない状態を保全）。
    pub safe_empty: bool,
}

/// アプリ状態（state.json）を復元する（要件10.1/13・design doc 9章/19章 状態復元3分岐）。
///
/// version 安全側・復元3分岐の判定はすべて pika-core::state。ここは FS 確認と DTO 変換のみ。
#[tauri::command]
pub fn restore_app_state() -> Result<RestoreOutcomeDto, String> {
    let root = state_store::resolve()?;
    let outcome = state_store::restore(&root);

    let (workspace_status, workspace_path) = match outcome.workspace {
        WorkspaceRestore::Restore(p) => ("restore".to_string(), Some(p)),
        WorkspaceRestore::EmptyState => ("empty-state".to_string(), None),
        WorkspaceRestore::NoWorkspace => ("no-workspace".to_string(), None),
    };

    let tabs = outcome
        .tabs
        .into_iter()
        .map(|t| match t {
            TabRestore::Restore(tab) => RestoredTabDto {
                status: "restore".into(),
                tab,
            },
            TabRestore::Deleted(tab) => RestoredTabDto {
                status: "deleted".into(),
                tab,
            },
            TabRestore::Unread(tab) => RestoredTabDto {
                status: "unread".into(),
                tab,
            },
        })
        .collect();

    Ok(RestoreOutcomeDto {
        workspace_status,
        workspace_path,
        tabs,
        safe_empty: outcome.safe_empty,
    })
}

/// 内容（バイト列）を LF 正規化してハッシュ化する（自己保存抑制の保存後ハッシュ）。
/// 改行のみの差を未読/差分に出さない方針（要件8.1）と整合させる。
fn hash_normalized(bytes: &[u8]) -> String {
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'\r' => {
                out.push(b'\n');
                if i + 1 < bytes.len() && bytes[i + 1] == b'\n' {
                    i += 1;
                }
            }
            b => out.push(b),
        }
        i += 1;
    }
    let mut h = twox_hash::XxHash64::with_seed(0);
    h.write(&out);
    format!("{:016x}", h.finish())
}
