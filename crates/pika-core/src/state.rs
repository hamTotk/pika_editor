//! アプリ状態の永続化と復元3分岐（要件10.1/13・design doc 9章/11章/19章 状態復元3分岐）。
//!
//! `state.json` はデータルート配下に置く（ワークスペースを汚さない）。
//! AppState（フォルダ/タブ/カーソル/スクロール/表示モード/差分トグル/ツリー展開/ウィンドウ状態）を
//! **アトミック書込**（一時ファイル→置換。実 FS は呼び出し側）し、起動時に復元する。
//!
//! 本モジュールは I/O を行わない純粋ロジック（cargo test の決定論ゲート対象）:
//! - [`AppState`] の serde 直列化・[`load_state`]（version 安全側のパース）。
//! - [`restore_tab`]：タブごとの**復元3分岐**（消失/別物/正常）の決定論判定。
//! - [`restore_workspace`]：ワークスペース消失時の安全遷移（空状態へ）。
//!
//! version 安全側（要件13・design doc 9章）:
//! - 既知 version のみ読む。**未知 version は読まず/書かず/再生成せず**安全側に倒す
//!   （読めない状態を壊さない＝最上位原則「データを失わない」のリポジトリ版）。

use crate::error::{PikaError, Result};
use crate::recent::RecentList;
use serde::{Deserialize, Serialize};

/// state.json のスキーマバージョン。未知バージョンは安全側に倒して読まない。
pub const STATE_VERSION: u32 = 1;

/// 表示モード（ソース/プレビュー/分割）。ui-design 8章の 3 モードに対応する。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ViewMode {
    #[default]
    Source,
    Preview,
    Split,
}

/// 1 タブ分の永続状態（カーソル/スクロール/表示モード/差分トグル）。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TabState {
    /// タブで開いていた絶対パス。
    pub path: String,
    /// 1 始まりのカーソル行。
    #[serde(default)]
    pub cursor_line: u32,
    /// 1 始まりのカーソル桁。
    #[serde(default)]
    pub cursor_column: u32,
    /// スクロール位置（行ベース近似）。
    #[serde(default)]
    pub scroll_top: u32,
    /// 表示モード。
    #[serde(default)]
    pub view_mode: ViewMode,
    /// 差分トグル ON/OFF。
    #[serde(default)]
    pub diff_on: bool,
    /// 保存時点の LF 正規化内容ハッシュ。復元時の「別物」判定に使う（未読復元の素）。
    #[serde(default)]
    pub content_hash: String,
}

/// ウィンドウ状態（位置/サイズ/最大化）。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct WindowState {
    #[serde(default)]
    pub x: i32,
    #[serde(default)]
    pub y: i32,
    #[serde(default)]
    pub width: u32,
    #[serde(default)]
    pub height: u32,
    #[serde(default)]
    pub maximized: bool,
}

/// アプリ状態（state.json の中身）。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct AppState {
    /// スキーマバージョン（未知は読まない）。
    pub version: u32,
    /// 開いていたワークスペース（フォルダ）の絶対パス。単体ファイルのみのときは `None`。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub workspace: Option<String>,
    /// 開いていたタブ群。
    #[serde(default)]
    pub tabs: Vec<TabState>,
    /// アクティブタブのインデックス。
    #[serde(default)]
    pub active_tab: usize,
    /// ツリーで展開していたフォルダの絶対パス群。
    #[serde(default)]
    pub expanded_dirs: Vec<String>,
    /// ウィンドウ状態。
    #[serde(default)]
    pub window: WindowState,
    /// 最近使ったファイル/フォルダ（要件10.2・各20件 LRU）。
    #[serde(default)]
    pub recent: RecentList,
}

impl AppState {
    /// 現行バージョンの空状態を作る（ワークスペース未オープン・タブなし）。
    pub fn empty() -> Self {
        AppState {
            version: STATE_VERSION,
            workspace: None,
            tabs: Vec::new(),
            active_tab: 0,
            expanded_dirs: Vec::new(),
            window: WindowState::default(),
            recent: RecentList::default(),
        }
    }

    /// `active_tab` インデックスが指すタブのパスを返す（復元後の再アクティブ化に使う）。
    ///
    /// タブを復元順（消失=削除済み等で順序がずれうる）に開いたあと、**インデックスでなくパスで**
    /// アクティブタブを指定し直すための解決関数（eval high: active_tab 往復欠落）。
    /// 範囲外（タブ無し・index 過大）は `None`（フロントは先頭/最後の挙動に委ねる）。
    pub fn active_tab_path(&self) -> Option<&str> {
        self.tabs.get(self.active_tab).map(|t| t.path.as_str())
    }

    /// アトミック書込用に JSON へ直列化する（実書込＝一時ファイル→置換は呼び出し側）。
    pub fn to_json(&self) -> Result<String> {
        serde_json::to_string_pretty(self)
            .map_err(|e| PikaError::InvalidArgument(format!("state.json の直列化に失敗: {e}")))
    }
}

/// state.json をパースした結果（version 安全側の分岐を観測可能にする）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum StateLoad {
    /// 既知バージョンを正常に読めた。
    Ok(AppState),
    /// 未知バージョン。読まず/書かず/再生成せず安全側に倒す（既存ファイルは保全）。
    UnknownVersion(u32),
    /// JSON として壊れている。破損として安全側に倒す（空状態で起動・既存は上書きしない）。
    Corrupt(String),
}

/// state.json の中身（文字列）を安全側に倒してパースする（要件13・design doc 9章）。
///
/// - 既知バージョン → `Ok(AppState)`。
/// - 未知バージョン → `UnknownVersion`（**読まず/書かず/再生成せず**。呼び出し側は上書きしない）。
/// - JSON 破損 → `Corrupt`（空状態で起動。既存ファイルは保全＝データを失わない）。
///
/// version は他フィールドより先に読む（未知 version のファイルを完全デシリアライズしようとして
/// スキーマ差異で別エラーになるのを避ける＝version だけ先に覗いて分岐する）。
pub fn load_state(json: &str) -> StateLoad {
    // まず version だけを覗く（部分パース）。これで未知バージョンと破損を切り分ける。
    let probe: std::result::Result<VersionProbe, _> = serde_json::from_str(json);
    let version = match probe {
        Ok(p) => p.version,
        Err(e) => return StateLoad::Corrupt(format!("version を読めない: {e}")),
    };
    if version != STATE_VERSION {
        return StateLoad::UnknownVersion(version);
    }
    match serde_json::from_str::<AppState>(json) {
        Ok(state) => StateLoad::Ok(state),
        Err(e) => StateLoad::Corrupt(format!("既知バージョンだが本体を読めない: {e}")),
    }
}

/// version だけを先に覗くための部分スキーマ（他フィールドは無視する）。
#[derive(Deserialize)]
struct VersionProbe {
    version: u32,
}

/// 復元時のパス状態（呼び出し側が FS を見て決める入力）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PathProbe {
    /// パスが存在しない（消失）。
    Missing,
    /// パスは存在するが内容ハッシュが保存時と異なる（別物＝外部変更された）。
    Changed,
    /// パスが存在し内容ハッシュも一致（保存時のまま）。
    Same,
}

/// タブ復元の3分岐結果（要件10.1・design doc 19章 状態復元3分岐）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TabRestore {
    /// 正常に復元（カーソル/スクロールも復元する）。
    Restore(TabState),
    /// ファイル消失＝削除済み表示で開く（ベースラインは保持。タブは残す）。
    Deleted(TabState),
    /// 別物（外部変更）＝未読として復元する（差分マークを付ける）。
    Unread(TabState),
}

/// 1 タブの復元3分岐を決定論判定する（要件10.1・design doc 19章）。
///
/// - `Missing` → 削除済み表示（タブを残して「削除済み」マーク。ベースラインは保持）。
/// - `Changed` → 未読復元（保存後に外部変更された＝差分マークを付けて開く）。
/// - `Same`    → 正常復元（カーソル/スクロール/モードを復元）。
pub fn restore_tab(tab: &TabState, probe: PathProbe) -> TabRestore {
    match probe {
        PathProbe::Missing => TabRestore::Deleted(tab.clone()),
        PathProbe::Changed => TabRestore::Unread(tab.clone()),
        PathProbe::Same => TabRestore::Restore(tab.clone()),
    }
}

/// ワークスペース復元の判定結果。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum WorkspaceRestore {
    /// ワークスペースを復元する（フォルダが存在）。
    Restore(String),
    /// ワークスペース消失＝空状態へ安全遷移する（タブは単体ファイル規則で個別に復元可）。
    EmptyState,
    /// もともとワークスペース無し（単体ファイルのみ）。
    NoWorkspace,
}

/// ワークスペース復元の安全遷移を決定論判定する（要件10.1・design doc 19章）。
///
/// ワークスペースが消失していたら空状態へ落とす（クラッシュさせない＝固まらない/データを失わない）。
/// タブ自体は単体ファイル規則で個別に [`restore_tab`] により復元できる。
pub fn restore_workspace(workspace: Option<&str>, dir_exists: bool) -> WorkspaceRestore {
    match workspace {
        None => WorkspaceRestore::NoWorkspace,
        Some(path) if dir_exists => WorkspaceRestore::Restore(path.to_string()),
        Some(_) => WorkspaceRestore::EmptyState,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_tab() -> TabState {
        TabState {
            path: r"C:\ws\note.md".into(),
            cursor_line: 10,
            cursor_column: 5,
            scroll_top: 100,
            view_mode: ViewMode::Split,
            diff_on: true,
            content_hash: "abcd0000".into(),
        }
    }

    #[test]
    fn 空状態は現行バージョンで生成される() {
        let s = AppState::empty();
        assert_eq!(s.version, STATE_VERSION);
        assert!(s.workspace.is_none());
        assert!(s.tabs.is_empty());
    }

    #[test]
    fn 直列化と復元が往復する() {
        let mut s = AppState::empty();
        s.workspace = Some(r"C:\ws".into());
        s.tabs.push(sample_tab());
        s.active_tab = 0;
        s.expanded_dirs.push(r"C:\ws\sub".into());
        s.recent.push_file(r"C:\ws\note.md");
        s.recent.push_folder(r"C:\ws");
        let json = s.to_json().unwrap();
        match load_state(&json) {
            StateLoad::Ok(loaded) => assert_eq!(loaded, s),
            other => panic!("Ok を期待したが {other:?}"),
        }
    }

    #[test]
    fn active_tab_path_がインデックスをパスへ解決する() {
        let mut s = AppState::empty();
        s.tabs.push(TabState {
            path: r"C:\ws\a.md".into(),
            ..sample_tab()
        });
        s.tabs.push(TabState {
            path: r"C:\ws\b.md".into(),
            ..sample_tab()
        });
        s.active_tab = 1;
        assert_eq!(s.active_tab_path(), Some(r"C:\ws\b.md"));
        // 範囲外は None（フロントが先頭等へフォールバック）。
        s.active_tab = 99;
        assert_eq!(s.active_tab_path(), None);
    }

    #[test]
    fn 旧バージョン_recent_欠落でも_default_で読める() {
        // recent フィールドが無い既存 state.json も #[serde(default)] で読める（後方互換）。
        let json = r#"{"version":1,"tabs":[],"active_tab":0}"#;
        match load_state(json) {
            StateLoad::Ok(s) => assert!(s.recent.files.is_empty() && s.recent.folders.is_empty()),
            other => panic!("Ok を期待したが {other:?}"),
        }
    }

    #[test]
    fn 未知バージョンは読まず安全側に倒す() {
        // version=999 は未知。読まず/書かず/再生成せず（既存ファイルを壊さない）。
        let json = r#"{"version":999,"workspace":"C:\\ws","tabs":[]}"#;
        match load_state(json) {
            StateLoad::UnknownVersion(v) => assert_eq!(v, 999),
            other => panic!("UnknownVersion を期待したが {other:?}"),
        }
    }

    #[test]
    fn version_欠落は破損扱い() {
        let json = r#"{"workspace":"C:\\ws"}"#;
        assert!(matches!(load_state(json), StateLoad::Corrupt(_)));
    }

    #[test]
    fn 壊れた_json_は破損扱いで空起動できる() {
        assert!(matches!(load_state("{not json"), StateLoad::Corrupt(_)));
        // 破損時は呼び出し側が空状態で起動する（既存ファイルは上書きしない方針）。
        let _ = AppState::empty();
    }

    #[test]
    fn 既知バージョンだが本体スキーマ不正は破損扱い() {
        // version は既知だが tabs が型違い（数値）→ 本体読めない＝破損。
        let json = r#"{"version":1,"tabs":123}"#;
        assert!(matches!(load_state(json), StateLoad::Corrupt(_)));
    }

    #[test]
    fn タブ復元_消失は削除済み表示() {
        let tab = sample_tab();
        match restore_tab(&tab, PathProbe::Missing) {
            TabRestore::Deleted(t) => assert_eq!(t.path, tab.path),
            other => panic!("Deleted を期待したが {other:?}"),
        }
    }

    #[test]
    fn タブ復元_別物は未読復元() {
        let tab = sample_tab();
        match restore_tab(&tab, PathProbe::Changed) {
            TabRestore::Unread(t) => assert_eq!(t.path, tab.path),
            other => panic!("Unread を期待したが {other:?}"),
        }
    }

    #[test]
    fn タブ復元_一致は正常復元() {
        let tab = sample_tab();
        match restore_tab(&tab, PathProbe::Same) {
            TabRestore::Restore(t) => {
                // カーソル/スクロール/モードを保持して復元する。
                assert_eq!(t.cursor_line, 10);
                assert_eq!(t.scroll_top, 100);
                assert_eq!(t.view_mode, ViewMode::Split);
                assert!(t.diff_on);
            }
            other => panic!("Restore を期待したが {other:?}"),
        }
    }

    #[test]
    fn ワークスペース消失は空状態へ安全遷移() {
        assert_eq!(
            restore_workspace(Some(r"C:\gone"), false),
            WorkspaceRestore::EmptyState
        );
    }

    #[test]
    fn ワークスペース存在は復元() {
        assert_eq!(
            restore_workspace(Some(r"C:\ws"), true),
            WorkspaceRestore::Restore(r"C:\ws".to_string())
        );
    }

    #[test]
    fn ワークスペース無しは単体ファイル扱い() {
        assert_eq!(
            restore_workspace(None, false),
            WorkspaceRestore::NoWorkspace
        );
    }
}
