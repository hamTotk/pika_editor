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
use pika_core::state::{
    load_state, restore_tab, restore_workspace, AppState, PathProbe, StateLoad, TabRestore,
    TabState, WorkspaceRestore,
};
use std::path::{Path, PathBuf};

/// データルート配下の state.json ファイル名。
const STATE_FILE: &str = "state.json";

/// 復元の総合結果（フロントへ渡す前の中間表現）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RestoreOutcome {
    /// ワークスペース復元判定。
    pub workspace: WorkspaceRestore,
    /// タブごとの復元3分岐。
    pub tabs: Vec<TabRestore>,
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

/// AppState をアトミックに書き込む（一時ファイル→rename。途中クラッシュで旧版を壊さない）。
pub fn save(root: &DataRoot, state: &AppState) -> Result<(), String> {
    std::fs::create_dir_all(&root.path).map_err(|e| format!("データルート作成に失敗: {e}"))?;
    let json = state.to_json().map_err(|e| format!("{e}"))?;
    let target = state_path(root);
    // 一時ファイルへ書いてから rename（同一ボリュームのアトミック置換・要件12.1）。
    let tmp = target.with_extension("json.tmp");
    std::fs::write(&tmp, json).map_err(|e| format!("一時ファイル書込に失敗: {e}"))?;
    std::fs::rename(&tmp, &target).map_err(|e| {
        // 置換に失敗したら一時ファイルを残さない（クリーンアップ）。
        let _ = std::fs::remove_file(&tmp);
        format!("state.json 置換に失敗: {e}")
    })?;
    Ok(())
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
    let tabs = state.tabs.iter().map(|t| restore_one_tab(t)).collect();
    RestoreOutcome {
        workspace,
        tabs,
        safe_empty: false,
    }
}

/// 1 タブの PathProbe を FS から作り core の3分岐判定を呼ぶ。
fn restore_one_tab(tab: &TabState) -> TabRestore {
    let probe = probe_path(tab);
    restore_tab(tab, probe)
}

/// FS を見てタブのパス状態を判定する（消失/別物/一致）。
fn probe_path(tab: &TabState) -> PathProbe {
    let path = Path::new(&tab.path);
    if !path.exists() {
        return PathProbe::Missing;
    }
    // 内容ハッシュ照合（保存時ハッシュがあるときのみ別物判定。無ければ一致扱い＝安全側）。
    if tab.content_hash.is_empty() {
        return PathProbe::Same;
    }
    match std::fs::read(path) {
        Ok(bytes) => {
            let now = hash_normalized(&bytes);
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

/// 内容を LF 正規化してハッシュ化する（保存時ハッシュと同じ規則＝改行のみ差は無視）。
/// commands.rs の hash_normalized と同一規則（自己保存抑制/未読判定の照合と整合）。
fn hash_normalized(bytes: &[u8]) -> String {
    use std::hash::Hasher;
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
