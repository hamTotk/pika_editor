//! Tauri command 層（薄い境界）。ロジックは pika-core に置き、ここは I/O 配線のみ。
//!
//! 本スプリント（sprint 1・最薄ループ）は中心体験① の貫通の土台として
//! フォルダ列挙/ファイル読込/保存の最小 command を提供する。
//! ツリー列挙の除外/自然順・エンコーディング往復・自己保存抑制は後続スプリントで
//! pika-core（workspace/document/watcher）へ移す（design doc 4章）。

use crate::snapshot::SnapshotService;
use crate::state_store;
use crate::watcher::WatcherService;
use pika_core::hashing::hash_normalized_lf;
use pika_core::state::{AppState, TabRestore, WorkspaceRestore};
use serde::{Deserialize, Serialize};
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
    access: State<'_, crate::access::AccessControl>,
) -> Result<Vec<TreeEntry>, String> {
    let dir = Path::new(&path);
    if !dir.is_dir() {
        return Err(format!("フォルダではありません: {path}"));
    }
    // アクセス制御のルートを張る（#5）。以後この配下の read_file/open_document/save_document が通る。
    access.set_root(&path);
    // 永続スナップショット領域を確定し、前回までの索引/退避をロードする（#3・再起動で揮発しない）。
    // capture_baseline の非クロバーが効くよう、ベースライン取得**より前**に呼ぶ。
    // data_root 解決/ロード失敗は致命にせず警告ログのみで継続する（永続化なしで従来どおり動く）。
    match state_store::resolve() {
        Ok(data_root) => {
            if let Err(e) = snapshot.set_workspace(&path, &data_root.path) {
                crate::diagnostic::record(
                    pika_core::diagnostic::LogLevel::Warn,
                    "snapshot",
                    "set_workspace",
                    Some(&path),
                    &e,
                );
            }
        }
        Err(e) => {
            crate::diagnostic::record(
                pika_core::diagnostic::LogLevel::Warn,
                "snapshot",
                "data_root",
                Some(&path),
                &e,
            );
        }
    }
    // 列挙・並び順は list_dir と同一規則を共有する（enumerate_dir）。
    let entries = enumerate_dir(dir)?;
    // 直下ファイルの差分ベースライン内容を取得する（全既読スタート＝要件8.1）。
    // 機密/10MB以上/画像はハッシュのみ（pika-core::snapshot::policy が判定）。
    // ロード済みベースラインがある path は capture_baseline がスキップする（非クロバー・#3）。
    for entry in &entries {
        if !entry.is_dir {
            capture_baseline_for(Path::new(&entry.path), &snapshot);
        }
    }

    // 監視を開始（ベースライン取得・外部変更の emit）。監視不能 FS はポーリングへ縮退する。
    watcher.watch_root(dir)?;
    Ok(entries)
}

/// 指定フォルダ直下のエントリ一覧を**副作用なし**で列挙する（ツリーの子フォルダ遅延展開・UI T2）。
///
/// `open_workspace` は監視開始（`watch_root`）とベースライン取得という**副作用**を持つため、
/// ツリーのサブフォルダ展開にそのまま流用すると監視ルートが子フォルダへ付け替わってしまう。
/// 子の段階展開は「ワークスペース内の純粋な一段列挙」なので、ここでは I/O を読み取りのみに限定し
/// 監視/スナップショットには一切触れない（design doc 3章: command 層は薄い境界）。
/// 列挙・並び順は `open_workspace` と同一規則（`enumerate_dir`）を共有する。
#[tauri::command]
pub fn list_dir(path: String) -> Result<Vec<TreeEntry>, String> {
    let dir = Path::new(&path);
    if !dir.is_dir() {
        return Err(format!("フォルダではありません: {path}"));
    }
    enumerate_dir(dir)
}

/// フォルダ直下のエントリを列挙し、安定順（フォルダ先・名前昇順）で返す。
/// `open_workspace`（監視開始＋ベースライン取得つき）と `list_dir`（副作用なし展開）で共有する。
/// 除外リスト/自然順/シンボリックリンク循環検出は後続スプリントで workspace モジュールへ移す。
fn enumerate_dir(dir: &Path) -> Result<Vec<TreeEntry>, String> {
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
    // 暫定の安定順（自然順は後続スプリントの workspace モジュールで実装）。
    entries.sort_by(|a, b| (b.is_dir, &a.name).cmp(&(a.is_dir, &b.name)));
    Ok(entries)
}

/// 差分のベースライン内容を取得する（要件8.1・#20/#54）。
///
/// 機密/画像/10MB以上（policy=HashOnly）は **内容を read せず**ハッシュのみ経路へ倒す
/// （機密ファイルの平文を read_to_string で展開しない＝#20）。StoreContent のときだけ全文を読み、
/// バイナリ等で読めなければ内容保存に適さないため空内容のまま渡す（#54・データ最小化）。
fn capture_baseline_for(path: &Path, snapshot: &SnapshotService) {
    use pika_core::snapshot::baseline_policy;
    let path_str = path.to_string_lossy().to_string();
    let size = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    // policy 判定を read より先に行う（HashOnly なら平文を一切読まない）。
    let content = if baseline_policy(&path_str, size).stores_content() {
        // StoreContent でも読めない（バイナリ等）なら空内容で渡す（#54）。
        std::fs::read_to_string(path).unwrap_or_default()
    } else {
        // HashOnly 方針（機密/画像/10MB以上）: 内容を読まない（#20 機密平文の展開回避）。
        String::new()
    };
    snapshot.capture_baseline(&path_str, &content, size);
}

/// ファイル内容を読む（エンコーディング自動判定・封じ込め・巨大ファイルガード）。
///
/// - #5: `access.verify_read` で「ワークスペース配下/許可ファイル」へ封じ込め、任意パスを塞ぐ。
/// - #7: BOM/UTF-16/Shift_JIS を判定してデコードする（read_to_string の UTF-8 固定で Shift_JIS が
///   破損していた欠陥の修正）。判定は pika-core::encoding（cargo test 済み）。
/// - 巨大ファイルガード: 500MB 上限超は読み込まない（OOM 回避＝固まらない）。
#[tauri::command]
pub fn read_file(
    path: String,
    access: State<'_, crate::access::AccessControl>,
) -> Result<String, String> {
    let canon = access.verify_read(&path)?;
    let size = std::fs::metadata(&canon)
        .map_err(|e| e.to_string())?
        .len();
    if !pika_core::huge::FileStage::from_size(size).can_open() {
        return Err("ファイルが大きすぎて読み込めません".into());
    }
    let bytes = std::fs::read(&canon).map_err(|e| e.to_string())?;
    Ok(pika_core::encoding::decode(&bytes).text)
}

/// 最近使った項目の種別（ファイル/フォルダ）。
#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum RecentKind {
    File,
    Folder,
}

/// 最近使った項目（ファイル/フォルダ）を追記し、更新後のリストを返す（要件10.2・design doc 9章）。
///
/// LRU・重複排除・上限切り詰めは pika_core::recent（cargo test 済み）。ここは state.json の
/// read-modify-write（既存状態を読み→core でマージ→アトミック書込）と、ジャンプリスト反映
/// （Windows COM・系統C）への受け渡しのみを行う。state.json が未知バージョン/破損のときは
/// 既存状態を保全して何もしない（最上位原則「データを失わない」＝Preserved を返す）。
#[tauri::command]
pub fn note_recent(
    kind: RecentKind,
    path: String,
) -> Result<pika_core::recent::RecentList, String> {
    // state.json の RMW を直列化（#21 lost update 対策）。save_app_state と同一ロックで保護する。
    let _io = state_store::lock_io();
    let root = state_store::resolve()?;
    match state_store::load_for_update(&root) {
        // 既知バージョン or 初回（ファイル無し）→ 追記して保存する。
        state_store::LoadForUpdate::Editable(mut state) => {
            match kind {
                RecentKind::File => state.recent.push_file(&path),
                RecentKind::Folder => state.recent.push_folder(&path),
            }
            state_store::save(&root, &state)?;
            // OS のジャンプリスト（Recent）へも反映する（実描画は系統C・ベストエフォート）。
            crate::jumplist::add_recent(&path);
            Ok(state.recent)
        }
        // 未知バージョン/破損→上書きしない（保全）。空のリストを返す（フロントは表示更新しない）。
        state_store::LoadForUpdate::Preserve => Ok(pika_core::recent::RecentList::default()),
    }
}

/// パスの種別を返す（"file" | "dir" | "missing"）。
///
/// 存在しないファイルパスを「保存時に作成される新規タブ」として開く判断（要件3.2）に使う。
/// 存在判定のみで内容は読まない（軽い）。
#[tauri::command]
pub fn path_kind(path: String) -> String {
    let p = Path::new(&path);
    if p.is_dir() {
        "dir".into()
    } else if p.is_file() {
        "file".into()
    } else {
        "missing".into()
    }
}

/// 内容の LF 正規化ハッシュを返す（state.json のタブ content_hash 算出・要件10.1/13）。
///
/// 自己保存抑制（save_document）・復元の別物判定（state_store::probe_path）と**同一規則**
/// （pika_core::hashing）で計算する。フロントは開く/保存/外部リロード時にこれを呼んで
/// タブの content_hash を実値で詰める（eval high: ダミー値固定の解消）。算出は cheap で
/// 開く/保存時のみ呼ぶためホットパスではない。
#[tauri::command]
pub fn hash_content(content: String) -> String {
    hash_normalized_lf(content.as_bytes())
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
///
/// `recent`（最近使った項目）は note_recent が read-modify-write で**単独管理**する owner なので、
/// フロントが送る AppState.recent では上書きせず**ディスクの既存値を保持**する
/// （フロントは recent を空で送る規約。二重管理による取りこぼしを構造的に防ぐ）。
#[tauri::command]
pub fn save_app_state(mut state: AppState) -> Result<(), String> {
    // state.json の RMW を直列化（#21 lost update 対策）。note_recent と同一ロックで保護する。
    let _io = state_store::lock_io();
    let root = state_store::resolve()?;
    if let state_store::LoadForUpdate::Editable(existing) = state_store::load_for_update(&root) {
        state.recent = existing.recent;
    }
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
    /// 復元後にアクティブ化すべきタブの絶対パス（`active_tab` インデックスを解決した値）。
    /// 復元順に依らずパスで再アクティブ化するため（eval high: active_tab 往復欠落の解消）。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub active_path: Option<String>,
    /// 未知バージョン/破損で空起動した（このセッションは上書き保存を控える＝読めない状態を保全）。
    pub safe_empty: bool,
}

/// アプリ状態（state.json）を復元する（要件10.1/13・design doc 9章/19章 状態復元3分岐）。
///
/// version 安全側・復元3分岐の判定はすべて pika-core::state。ここは FS 確認と DTO 変換のみ。
#[tauri::command]
pub fn restore_app_state(
    access: State<'_, crate::access::AccessControl>,
) -> Result<RestoreOutcomeDto, String> {
    let root = state_store::resolve()?;
    let outcome = state_store::restore(&root);

    let (workspace_status, workspace_path) = match outcome.workspace {
        WorkspaceRestore::Restore(p) => ("restore".to_string(), Some(p)),
        WorkspaceRestore::EmptyState => ("empty-state".to_string(), None),
        WorkspaceRestore::NoWorkspace => ("no-workspace".to_string(), None),
    };

    // 復元するワークスペース/タブをアクセス制御に許可する（#5）。後続の openFile/openDocument が
    // 通るよう、DTO を返す**前に**登録する（frontend が復元タブを開くより必ず先になる）。
    if let Some(ws) = &workspace_path {
        access.set_root(ws);
    }

    let tabs: Vec<RestoredTabDto> = outcome
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

    // 各復元タブのパスを個別許可する（単体ファイル復元や workspace 外のタブも開けるように）。
    for t in &tabs {
        access.allow_file(&t.tab.path);
    }

    Ok(RestoreOutcomeDto {
        workspace_status,
        workspace_path,
        tabs,
        active_path: outcome.active_path,
        safe_empty: outcome.safe_empty,
    })
}

// ── 「OS 既定アプリで開く」導線（要件6.2・design G）─────────────────────────────
//
// 【セキュリティ境界・最小権限】opener plugin の command ACL は **frontend へ一切開放しない**
// （capabilities/main.json は core:default のみ・opener permission は付与しない）。frontend は
// ここの自前 #[tauri::command] を invoke するだけで、その内部からのみ opener の Rust API
// （`open_path`）を呼ぶ。これにより「任意 URL/任意パスを開く汎用導線」を作らず、対象を
// **現在開いているファイル / pika のログフォルダ** に限定できる（design doc 15章「足さない」）。
// 別WebView（プレビュー・権限ゼロ）からの到達は main.rs の発信元ラベル検査で全拒否される
// （自前 command も choke point で塞がれる＝多層防御）。

/// パスを「OS 既定アプリで開いてよいか」を backend 側で再検証する（fail-closed）。
///
/// frontend からの値は信頼せず、ここで **絶対パス** かつ **実在** を確認する（不正は拒否）。
/// 相対パス・存在しないパスは開かない（要件: パス妥当性チェック・最上位原則の防御的入力検証）。
/// 種別（ファイル/フォルダ）は問わない（既定アプリ＝ファイルなら関連付けアプリ、フォルダなら
/// エクスプローラーで開く）。
fn validate_openable(path: &str) -> Result<&Path, String> {
    let p = Path::new(path);
    if !p.is_absolute() {
        return Err("絶対パスではありません".to_string());
    }
    if !p.exists() {
        return Err("対象が存在しません".to_string());
    }
    Ok(p)
}

/// 指定ファイル/フォルダを OS 既定アプリで開く（要件6.2・design G「ブラウザで開く」の正規導線）。
///
/// frontend は **現在アクティブなタブのファイルパス** だけを渡す規約（任意 URL は受けない）。
/// backend でも絶対パス＋実在を再検証し（[`validate_openable`]）、不正は Err で拒否する（fail-closed）。
/// `open_path(.., None)` は既定アプリで detached 起動するため UI スレッドを塞がない（固まらない）。
#[tauri::command]
pub fn open_in_default_app(path: String) -> Result<(), String> {
    let p = validate_openable(&path)?;
    // with=None で OS 既定アプリ（HTML なら既定ブラウザ等）。detached 起動。
    // 接頭辞は frontend で一元付与するため、ここでは素の原因のみ返す（F-027 二重接頭辞回避）。
    tauri_plugin_opener::open_path(p, None::<&str>).map_err(|e| e.to_string())
}

/// 診断ログフォルダ（`<データルート>/logs/`）を OS（エクスプローラー）で開く（要件12.3・design G）。
///
/// パスは backend が確定（無ければ作成）する（[`crate::diagnostic::log_folder_path`] と同じ算出）。
/// frontend からパスを受け取らない＝対象を pika のログフォルダに固定し、任意フォルダ閲覧の導線にしない。
#[tauri::command]
pub fn open_log_folder() -> Result<(), String> {
    // ログフォルダのパス確定＋作成は診断モジュールに集約（重複算出を避ける）。
    let dir = crate::diagnostic::log_folder_path()?;
    let p = validate_openable(&dir)?;
    tauri_plugin_opener::open_path(p, None::<&str>).map_err(|e| e.to_string())
}
