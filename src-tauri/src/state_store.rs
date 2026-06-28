//! state.json のアトミック書込と復元の FS 配線（要件10.1/13・design doc 9章/11章）。
//!
//! 役割（design doc 3章「薄い橋渡し層」）:
//! - 復元3分岐・version 安全側・直列化の純粋ロジックは **pika-core::state**（cargo test 済み）に委ねる。
//! - ここには「データルート解決→state.json のアトミック書込（一時ファイル→置換）→
//!   FS を見て PathProbe を作り core の復元判定を呼ぶ」の I/O のみを置く。
//!
//! アトミック書込（要件12.1「一時ファイル→置換」）でクラッシュ時も旧 state.json を壊さない
//! （最上位原則「データを失わない」）。**未知バージョン/破損時は上書きしない**（読めない状態を保全）。

use pika_core::data_root::{resolve_data_root, DataRoot};
use pika_core::hashing::{hash_normalized_lf, HUGE_FILE_THRESHOLD_BYTES};
use pika_core::state::{
    load_state, restore_tab, restore_workspace, AppState, PathProbe, StateLoad, TabRestore,
    TabState, WorkspaceRestore,
};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

/// データルート配下の state.json ファイル名。
const STATE_FILE: &str = "state.json";

/// state.json の read-modify-write を直列化するプロセス内ロック（#21 lost update 対策）。
///
/// `note_recent`（recent 追記）と `save_app_state`（タブ/ウィンドウ状態保存）は、いずれも
/// `load_for_update → 加工 → save` の RMW を行う。両者が並行すると後勝ちで一方の更新を取りこぼす
/// （recent が消える/タブ状態が巻き戻る）。RMW 区間全体をこのロックで囲んで不可分化する。
static STATE_IO_LOCK: Mutex<()> = Mutex::new(());

/// state.json の RMW を直列化するロックを取得する（#21）。
///
/// `note_recent` / `save_app_state` の冒頭で `let _g = state_store::lock_io();` として保持し、
/// load→save を不可分にする。ロックが毒化していても `into_inner` で続行する（保存を止めない）。
pub fn lock_io() -> std::sync::MutexGuard<'static, ()> {
    STATE_IO_LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

/// 復元の総合結果（フロントへ渡す前の中間表現）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RestoreOutcome {
    /// ワークスペース復元判定。
    pub workspace: WorkspaceRestore,
    /// タブごとの復元3分岐。
    pub tabs: Vec<TabRestore>,
    /// 復元後にアクティブ化すべきタブの絶対パス（`active_tab` インデックスを解決した値）。
    /// タブを復元順に開いたあと**パスで**アクティブを指定し直すため（eval high: active_tab 往復）。
    pub active_path: Option<String>,
    /// 未知バージョン/破損で空状態起動になったか（上書き禁止フラグ）。
    pub safe_empty: bool,
}

/// データルートを解決する（exe 隣 portable.txt → ポータブル、無ければ %LOCALAPPDATA%\pika）。
/// 解決ロジックは pika-core::data_root（cargo test 済み）。ここは FS/env の取得のみ。
pub fn resolve() -> Result<DataRoot, String> {
    let exe = std::env::current_exe().map_err(|e| format!("exe パス取得に失敗: {e}"))?;
    let exe_dir = exe
        .parent()
        .ok_or_else(|| "exe の親ディレクトリが取れない".to_string())?;
    let exe_dir_str = exe_dir.to_string_lossy().to_string();
    let portable_present = exe_dir.join("portable.txt").exists();
    let local_appdata = std::env::var("LOCALAPPDATA").ok();
    resolve_data_root(&exe_dir_str, portable_present, local_appdata.as_deref())
        .map_err(|e| format!("データルート解決に失敗: {e}"))
}

/// state.json の絶対パスを返す。
fn state_path(root: &DataRoot) -> PathBuf {
    root.path.join(STATE_FILE)
}

/// AppState をアトミックに書き込む（一時ファイル→fsync→単一アトミック置換。途中クラッシュで旧版を壊さない）。
///
/// 書込プリミティブ（一時ファイル PID＋連番固有名／`sync_all`（fsync）／単一アトミック置換／失敗時の
/// 一時ファイル後始末）は src-tauri 共通の [`crate::fs_atomic::atomic_write`] へ集約した。旧実装は
/// `std::fs::rename` 置換だったが、最安全の `MoveFileExW(MOVEFILE_REPLACE_EXISTING|WRITE_THROUGH)`
/// 単一置換へ**格上げ**統一している（消失窓を作らない＝最上位原則「データを失わない」の強化）。
pub fn save(root: &DataRoot, state: &AppState) -> Result<(), String> {
    std::fs::create_dir_all(&root.path).map_err(|e| format!("データルート作成に失敗: {e}"))?;
    let json = state.to_json().map_err(|e| format!("{e}"))?;
    let target = state_path(root);
    crate::fs_atomic::atomic_write(&target, json.as_bytes())
        .map_err(|e| format!("state.json の保存に失敗: {e}"))?;
    Ok(())
}

/// `note_recent` 等の read-modify-write 用に state.json を読んだ結果。
pub enum LoadForUpdate {
    /// 既知バージョン or 初回（ファイル無し＝空状態）。安全に追記・保存してよい。
    Editable(AppState),
    /// 未知バージョン/破損。**上書きしてはならない**（既存状態を保全＝データを失わない）。
    Preserve,
}

/// state.json を read-modify-write 用に読む（version 安全側）。
///
/// - ファイル不在 → 初回扱いで `Editable(空状態)`（追記して新規作成してよい）。
/// - 既知バージョン → `Editable(読めた状態)`。
/// - 未知バージョン/破損 → `Preserve`（読めない状態を上書きしない）。
pub fn load_for_update(root: &DataRoot) -> LoadForUpdate {
    let path = state_path(root);
    let json = match std::fs::read_to_string(&path) {
        Ok(j) => j,
        Err(_) => return LoadForUpdate::Editable(AppState::empty()),
    };
    match load_state(&json) {
        StateLoad::Ok(state) => LoadForUpdate::Editable(state),
        StateLoad::UnknownVersion(_) | StateLoad::Corrupt(_) => LoadForUpdate::Preserve,
    }
}

/// state.json を読み、core の復元判定（version 安全側・3分岐）を適用する。
///
/// - ファイル不在 → 初回起動とみなし空状態（safe_empty=false。書込は通常どおり許可）。
/// - 未知バージョン/破損 → 空状態だが **safe_empty=true**（呼び出し側は上書きしない＝読めない状態を保全）。
/// - 正常 → ワークスペース/各タブを FS で確認し復元3分岐を適用する。
pub fn restore(root: &DataRoot) -> RestoreOutcome {
    let path = state_path(root);
    let json = match std::fs::read_to_string(&path) {
        Ok(j) => j,
        Err(_) => {
            // 初回起動（state.json なし）。空状態だが上書きは許可する。
            return RestoreOutcome {
                workspace: WorkspaceRestore::NoWorkspace,
                tabs: Vec::new(),
                active_path: None,
                safe_empty: false,
            };
        }
    };

    match load_state(&json) {
        StateLoad::Ok(state) => apply_restore(&state),
        StateLoad::UnknownVersion(_) | StateLoad::Corrupt(_) => {
            // 安全側: 空状態で起動しつつ、既存 state.json は上書きしない（safe_empty）。
            RestoreOutcome {
                workspace: WorkspaceRestore::NoWorkspace,
                tabs: Vec::new(),
                active_path: None,
                safe_empty: true,
            }
        }
    }
}

/// 正常に読めた AppState に対し、FS を確認しながら復元3分岐を適用する。
fn apply_restore(state: &AppState) -> RestoreOutcome {
    let workspace = restore_workspace(
        state.workspace.as_deref(),
        state
            .workspace
            .as_deref()
            .map(|p| Path::new(p).is_dir())
            .unwrap_or(false),
    );
    let tabs = state.tabs.iter().map(restore_one_tab).collect();
    // active_tab インデックスをパスへ解決（復元順がずれてもパスで再アクティブ化できる）。
    let active_path = state.active_tab_path().map(|s| s.to_string());
    RestoreOutcome {
        workspace,
        tabs,
        active_path,
        safe_empty: false,
    }
}

/// 1 タブの PathProbe を FS から作り core の3分岐判定を呼ぶ。
fn restore_one_tab(tab: &TabState) -> TabRestore {
    let probe = probe_path(tab);
    restore_tab(tab, probe)
}

/// FS を見てタブのパス状態を判定する（消失/別物/一致）。
///
/// 起動0.5秒ゲートのホットパス（復元タブ分だけ直列に走る）なので、
/// **10MB 以上のファイルは全量を読み直さず一致扱い**にする（eval high/performance:
/// probe_path の巨大ファイルガード）。spec「10MB 以上はハッシュのみ・内容保存しない」と整合し、
/// 巨大テキストを複数タブ復元しても起動を全量読込でブロックしない。
fn probe_path(tab: &TabState) -> PathProbe {
    let path = Path::new(&tab.path);
    let meta = match std::fs::metadata(path) {
        Ok(m) => m,
        // メタも取れない＝存在しない/権限なし。存在判定は exists() に委ねる。
        Err(_) => {
            if path.exists() {
                return PathProbe::Same; // 権限等で読めないだけなら未読を立てない（安全側）。
            }
            return PathProbe::Missing;
        }
    };
    if !path.exists() {
        return PathProbe::Missing;
    }
    // 内容ハッシュ照合（保存時ハッシュがあるときのみ別物判定。無ければ一致扱い＝安全側）。
    if tab.content_hash.is_empty() {
        return PathProbe::Same;
    }
    // 巨大ファイルは全量読込せず一致扱い（起動ホットパスを保護＝spec 10MB ハッシュのみ）。
    if meta.len() >= HUGE_FILE_THRESHOLD_BYTES {
        return PathProbe::Same;
    }
    match std::fs::read(path) {
        Ok(bytes) => {
            let now = hash_normalized_lf(&bytes);
            if now == tab.content_hash {
                PathProbe::Same
            } else {
                PathProbe::Changed
            }
        }
        // 読めない（権限等）は安全側で「別物」とせず一致扱い（誤って未読を立てない）。
        Err(_) => PathProbe::Same,
    }
}
