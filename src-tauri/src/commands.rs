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
    settings: State<'_, std::sync::Arc<crate::settings_service::SettingsService>>,
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
    // 列挙・並び順は list_dir と同一規則を共有する（enumerate_dir）。除外ディレクトリ（settings.toml
    // excluded_dirs・既定 .git/node_modules）はここで弾く。除外されたディレクトリ配下は列挙されないため
    // 下の baseline ループ（除外後の entries に対して回る）にも lazy 展開にも自然に乗らない。
    let settings_now = settings.snapshot();
    let excluded = settings_now.excluded_dirs;
    let entries = enumerate_dir(dir, &excluded)?;
    // 設定 sensitive_patterns（機密判定の和集合）を baseline 判定へ流し込む（baseline ループ前＝U2b-2）。
    // 以後 SnapshotInner の baseline_policy_with と pre-check（capture_baseline_for）が同じ patterns で
    // 一致する。**既定機密は外せない**（is_sensitive_with が常に内包）＝設定は足すだけ（減らせない不変条件）。
    let sensitive_patterns = settings_now.sensitive_patterns;
    snapshot.set_sensitive_patterns(sensitive_patterns.clone());
    // 直下ファイルの差分ベースライン内容を取得する（全既読スタート＝要件8.1）。
    // 機密/10MB以上/画像はハッシュのみ（pika-core::snapshot::policy が判定）。
    // ロード済みベースラインがある path は capture_baseline がスキップする（非クロバー・#3）。
    //
    // 性能（設計原則2「固まらない」）: ここは UI 起点・command スレッドで N 件ループする。各 capture が
    // index.json をフル直列化＋fsync すると N 回の超線形コストでフォルダオープンが 200ms 予算を超えうる。
    // そこで capture_baseline は内容 object のみ即時永続化し index 永続化は遅延、
    // ループ完了後に persist_index を **1回だけ**呼んで index.json をまとめて書く（confirm_all と同じ作法）。
    for entry in &entries {
        if !entry.is_dir {
            capture_baseline_for(Path::new(&entry.path), &snapshot, &sensitive_patterns);
        }
    }
    // バッチ capture で遅延していた index 永続化をここで1回だけ行う（per-file fsync の解消）。
    // クラッシュ耐性: 仮にここへ到達前にクラッシュしても内容 object/メタは各 capture で永続化済みなので、
    // 次回 open の再 capture（非クロバー）＋recover_from_meta で復元できる安全網は崩れない。
    snapshot.persist_index();

    // 監視の除外をツリー列挙（enumerate_dir）と同じ excluded_dirs に揃える。
    // watch_root が baseline 列挙で inner.excluded_dirs を読むため、**watch_root より前**に設定する
    // （これまで watcher は .git/node_modules をハードコードし、dist/target を足すとツリーから消えるのに
    // fs-changed が飛び続け未読が消せない不整合があった＝解消）。`excluded` はここ以降未使用なので move する。
    watcher.set_excluded_dirs(excluded);
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
pub fn list_dir(
    path: String,
    settings: State<'_, std::sync::Arc<crate::settings_service::SettingsService>>,
) -> Result<Vec<TreeEntry>, String> {
    let dir = Path::new(&path);
    if !dir.is_dir() {
        return Err(format!("フォルダではありません: {path}"));
    }
    // サブフォルダ展開でも open_workspace と同じ除外（excluded_dirs）を効かせる。
    enumerate_dir(dir, &settings.snapshot().excluded_dirs)
}

/// 除外ディレクトリ判定（settings.toml `excluded_dirs` をツリー列挙へ配線）。
///
/// `excluded_dirs`（既定 `[".git","node_modules"]`）は**ディレクトリ名**を意味するので、
/// `is_dir == true` のときだけ判定する（同名のファイルは除外しない）。一致は Windows のパス大小無視に
/// 合わせて**大文字小文字を無視**する（`.git` も `.GIT` も同一視）。
/// MVP のため判定は直下名の完全一致のみ（glob・パス全体マッチは対象外＝design doc 15章「足さない」）。
fn is_excluded_dir(name: &str, is_dir: bool, excluded: &[String]) -> bool {
    is_dir && excluded.iter().any(|e| e.eq_ignore_ascii_case(name))
}

/// フォルダ直下のエントリを列挙し、安定順（フォルダ先・名前昇順）で返す。
/// `open_workspace`（監視開始＋ベースライン取得つき）と `list_dir`（副作用なし展開）で共有する。
/// `excluded`（settings.toml の `excluded_dirs`）に名前が一致するディレクトリは列挙から除外する。
/// 自然順/シンボリックリンク循環検出は後続スプリントで workspace モジュールへ移す。
fn enumerate_dir(dir: &Path, excluded: &[String]) -> Result<Vec<TreeEntry>, String> {
    let mut entries = Vec::new();
    let read = std::fs::read_dir(dir).map_err(|e| format!("読み取りに失敗: {e}"))?;
    for ent in read.flatten() {
        let p = ent.path();
        let name = ent.file_name().to_string_lossy().to_string();
        let is_dir = p.is_dir();
        // 除外ディレクトリ（.git/node_modules 等）は列挙しない＝lazy 展開も監視もベースラインも対象外になる。
        if is_excluded_dir(&name, is_dir, excluded) {
            continue;
        }
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

/// 差分のベースライン内容を取得する（要件8.1・#20/#54・指摘6）。
///
/// 機密/画像/10MB以上（policy=HashOnly）は **内容を read せず**ハッシュのみ経路へ倒す
/// （機密ファイルの平文を read_to_string で展開しない＝#20）。StoreContent のときだけバイト読み→
/// **open_document と同じ encoding::decode** でデコードしてテキスト内容を保存する（Shift_JIS/UTF-16/
/// UTF-8 BOM を含め baseline ソースを editor 内容＝decoded.text に揃える＝指摘6）。
/// 読取失敗はベースラインを張らない（空文字を内容ベースライン化しない＝差分誤り/空ハッシュ衝突を防ぐ・#54）。
///
/// open_workspace のループから呼ばれる。capture_baseline は内容 object を即時永続化するが index.json は
/// ここで書かず、呼び出し側がループ後に snapshot.persist_index() で1回まとめて永続化する
/// （per-file の index 直列化＋fsync を避ける＝固まらない・設計原則2）。
fn capture_baseline_for(path: &Path, snapshot: &SnapshotService, sensitive_patterns: &[String]) {
    use pika_core::snapshot::baseline_policy_with;
    let path_str = path.to_string_lossy().to_string();
    let size = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    // index 永続化はループ後に1回（capture_* は index を遅延し object のみ即時永続化）＝per-file fsync を避ける。
    // policy 判定を read より先に行う（HashOnly なら平文を一切読まない）。
    // 機密判定は設定 sensitive_patterns 和集合（既定は外せない＝U2b-2）。pre-check と SnapshotInner 内の
    // 判定（snapshot.set_sensitive_patterns 済み）を同じ patterns で揃え、機密ファイルの平文を読まない。
    if !baseline_policy_with(&path_str, size, sensitive_patterns).stores_content() {
        // 機密/画像/10MB以上: 内容を読まない（#20）。ハッシュのみ（content 空＝未読検知は watcher が担う）。
        snapshot.capture_baseline(&path_str, "", size);
        return;
    }
    match std::fs::read(path) {
        Ok(bytes) => {
            // **open_document と同じ encoding::decode** で判定する（指摘6 の direct-children 漏れ）。
            // 旧実装は String::from_utf8 を使い、Shift_JIS/UTF-16（非UTF-8テキスト）が Err→ハッシュのみへ倒れて
            // 直下ファイルが誤って「差分非対象」になり、UTF-8 BOM はベースラインに BOM が残って未編集でも偽差分が
            // 出ていた。decode は BOM を剥がし元エンコーディングを判定するので、テキストは baseline ソースが editor
            // 内容（open_document の decoded.text / ensure_baseline）と一致し、直下/サブフォルダ・全エンコーディングで
            // 差分が一貫する（機密/巨大/画像は上の policy 早期 return で既に除外済み）。
            let decoded = pika_core::encoding::decode(&bytes);
            if decoded.had_decode_warning {
                // strict UTF-8 も Shift_JIS 等も判定できず lossy デコードへ倒れた＝テキストでない（バイナリ）。
                // 旧 from_utf8 Err 分岐の意図（内容を保存しない）を復元する: ロッシーデコードしたバイナリ内容を
                // data root へ保存せず、バイト由来ハッシュのみベースライン化する（データ最小化#20・肥大/
                // folder-open コスト回避・第2巡 回帰修正。空文字で確定しない＝#54）。
                let hash = hash_normalized_lf(&bytes);
                snapshot.capture_baseline_hash_only(&path_str, &hash);
            } else {
                // テキスト（UTF-8/Shift_JIS/UTF-16・BOM 除去済み）: デコード済み内容を保存（差分・巻き戻し可能）。
                snapshot.capture_baseline(&path_str, &decoded.text, size);
            }
        }
        // 読めない（権限/一時ロック等）: ベースラインを張らない（次回 open で再試行。空文字で確定しない・#54）。
        Err(_) => {}
    }
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
        // #52: ワークスペースが一時不在（ドライブ未接続等）のとき、incoming が workspace=None でも
        // disk の参照を保全する（上書きで参照を失わない）。frontend が別フォルダを開けば
        // workspace=Some(新) で来るのでそれは尊重する＝トラップにならない。safe_empty で全保存を
        // 止める方式は採らない（永続的トラップ化を避ける・backend-only の保全に留める）。
        if state.workspace.is_none() {
            if let Some(ws) = existing.workspace.clone() {
                // disk のワークスペースが「設定済みだが現在ディレクトリとして存在しない」＝一時不在の
                // ときだけ保全する（現存するなら incoming が None になる正常フローは無い）。
                if !std::path::Path::new(&ws).is_dir() {
                    state.workspace = Some(ws);
                }
            }
        }
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

// ── ツリーからのファイル/フォルダ 新規作成・削除（要件11「新規ファイル/新規フォルダ/削除」・design G）──
//
// 【セキュリティ境界】frontend からのパスは信頼せず、AccessControl で**開いているワークスペース配下**へ
// 封じ込める（任意パスへの作成/削除を塞ぐ＝#5/#46）。削除は**完全削除でなくごみ箱へ移動**して復元可能性を
// 残す（要件11「Delete＝ごみ箱へ移動」・最上位原則「データを失わない」）。別WebView（プレビュー・権限
// ゼロ）からの到達は main.rs の発信元ラベル検査で全拒否される。

/// 新規作成する名前を検証する（ファイル名のみ・パス脱出や予約文字の混入を防ぐ）。
///
/// `dir` への `join` 前にこれを通し、`..`/区切り文字/Windows 予約文字/制御文字を弾く（封じ込めの一次防御。
/// 最終的な配下確認は AccessControl::verify_write が親 canonicalize で行う＝多層防御）。
fn validate_entry_name(name: &str) -> Result<(), String> {
    let n = name.trim();
    if n.is_empty() {
        return Err("名前を入力してください".to_string());
    }
    if n == "." || n == ".." {
        return Err("その名前は使用できません".to_string());
    }
    if n.contains('/') || n.contains('\\') {
        return Err("名前にパス区切り文字（/ \\）は使えません".to_string());
    }
    // Windows で使用不可の文字（: * ? " < > |）と制御文字を弾く。
    if n.chars()
        .any(|c| (c as u32) < 0x20 || matches!(c, ':' | '*' | '?' | '"' | '<' | '>' | '|'))
    {
        return Err("名前に使用できない文字が含まれています".to_string());
    }
    Ok(())
}

/// ツリーから新規ファイル/フォルダを作る（要件11・design G 右クリックメニュー）。
///
/// `dir` 直下に `name` の空ファイル（`is_dir=false`）/フォルダ（`is_dir=true`）を作成し、作成した絶対パスを
/// 返す（frontend がツリー更新＋新規ファイルを開くのに使う）。AccessControl::verify_write で**親（=dir）が
/// ワークスペース配下**かを検証し、任意パスへの作成を塞ぐ。同名が既にあればエラー（既存を上書きしない）。
#[tauri::command]
pub fn create_entry(
    dir: String,
    name: String,
    is_dir: bool,
    access: State<'_, crate::access::AccessControl>,
) -> Result<String, String> {
    validate_entry_name(&name)?;
    let target = Path::new(&dir).join(name.trim());
    let target_str = target.to_string_lossy().to_string();
    // 親（=dir）がワークスペース配下かを検証する（新規パスは親 canonicalize で封じ込め＝#46）。
    access.verify_write(&target_str)?;
    if target.exists() {
        return Err(format!(
            "同名の{}が既に存在します",
            if is_dir { "フォルダ" } else { "ファイル" }
        ));
    }
    if is_dir {
        std::fs::create_dir(&target).map_err(|e| format!("フォルダの作成に失敗しました: {e}"))?;
    } else {
        // 空ファイルを作成（既存は上書きしない＝create_new で競合を弾く）。
        std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&target)
            .map_err(|e| format!("ファイルの作成に失敗しました: {e}"))?;
    }
    Ok(target_str)
}

/// ツリーから削除する（要件11「Delete＝ごみ箱へ移動」・design G）。
///
/// **完全削除ではなくごみ箱へ移動**し、復元可能性を残す（最上位原則「データを失わない」）。
/// AccessControl::verify_write で対象がワークスペース配下かを検証してから OS のごみ箱へ送る。
/// 開いているファイルを削除した場合のタブ追従（「削除済み」表示）は watcher の removed イベントが担う。
#[tauri::command]
pub fn delete_entry(
    path: String,
    access: State<'_, crate::access::AccessControl>,
) -> Result<(), String> {
    // verify_write は親を canonicalize して封じ込め判定する（既存ファイル/フォルダの親＝配下確認・#46）。
    access.verify_write(&path)?;
    if !Path::new(&path).exists() {
        return Err("対象が存在しません".to_string());
    }
    move_to_recycle_bin(&path)
}

/// パスを OS のごみ箱へ移動する（Windows: SHFileOperationW + FOF_ALLOWUNDO）。
///
/// 完全削除を避けて復元可能性を残す（要件11・最上位原則）。確認 UI は frontend 側で出すため
/// OS の確認/進捗 UI は抑止する（FOF_NOCONFIRMATION/FOF_SILENT/FOF_NOERRORUI）。
#[cfg(windows)]
fn move_to_recycle_bin(path: &str) -> Result<(), String> {
    use windows_sys::Win32::UI::Shell::{
        SHFileOperationW, FOF_ALLOWUNDO, FOF_NOCONFIRMATION, FOF_NOERRORUI, FOF_SILENT,
        FOF_WANTNUKEWARNING, FO_DELETE, SHFILEOPSTRUCTW,
    };
    // pFrom は二重 NUL 終端の UTF-16（ファイルリストの終端としてもう1つ NUL を足す）。
    let mut from: Vec<u16> = path.encode_utf16().collect();
    from.push(0);
    from.push(0);
    // SAFETY: `from` は呼び出し中ずっと生存し二重 NUL 終端。`op` は zeroed 後に必要フィールドのみ設定する。
    let mut op: SHFILEOPSTRUCTW = unsafe { std::mem::zeroed() };
    op.wFunc = FO_DELETE as u32;
    op.pFrom = from.as_ptr();
    // FOF_WANTNUKEWARNING を立て、**ごみ箱へ入れられない対象**（ごみ箱を持たないネットワーク/リムーバブル
    // ドライブ・容量超過等で FOF_ALLOWUNDO が完全削除へフォールバックするケース）では OS 警告を出す（指摘3）。
    // これが無いと FOF_NOCONFIRMATION により無確認の完全削除に化け、フロントの「ごみ箱へ移動＝復元可能」の
    // 約束が破れる（最上位原則「データを失わない」）。通常のごみ箱移動では FOF_NOCONFIRMATION が効き無確認のまま。
    op.fFlags =
        (FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT | FOF_NOERRORUI) as u16;
    let rc = unsafe { SHFileOperationW(&mut op) };
    if rc != 0 {
        return Err(format!("ごみ箱へ移動できませんでした（コード {rc}）"));
    }
    if op.fAnyOperationsAborted != 0 {
        return Err("削除が中断されました".to_string());
    }
    Ok(())
}

/// 非 Windows ではごみ箱移動 API を持たない（pika は Windows 専用＝通常到達しない）。
#[cfg(not(windows))]
fn move_to_recycle_bin(_path: &str) -> Result<(), String> {
    Err("この環境ではごみ箱への移動に対応していません".to_string())
}

#[cfg(test)]
mod tests {
    use super::is_excluded_dir;
    use super::validate_entry_name;

    /// settings.toml の既定除外リスト（`.git` / `node_modules`）。
    fn excluded() -> Vec<String> {
        vec![".git".to_string(), "node_modules".to_string()]
    }

    #[test]
    fn 既定の_git_ディレクトリは除外される() {
        assert!(is_excluded_dir(".git", true, &excluded()));
        assert!(is_excluded_dir("node_modules", true, &excluded()));
    }

    #[test]
    fn 大文字小文字を無視して一致する() {
        // Windows のパス大小無視に合わせ、.GIT も除外する。
        assert!(is_excluded_dir(".GIT", true, &excluded()));
        assert!(is_excluded_dir("Node_Modules", true, &excluded()));
    }

    #[test]
    fn 同名のファイルは除外しない() {
        // excluded_dirs はディレクトリ名の意味なので、同名ファイル（is_dir=false）は弾かない。
        assert!(!is_excluded_dir("node_modules", false, &excluded()));
        assert!(!is_excluded_dir(".git", false, &excluded()));
    }

    #[test]
    fn 除外リストに無いディレクトリは残る() {
        assert!(!is_excluded_dir("src", true, &excluded()));
        assert!(!is_excluded_dir("docs", true, &excluded()));
    }

    #[test]
    fn 通常のファイル名は許可される() {
        assert!(validate_entry_name("memo.md").is_ok());
        assert!(validate_entry_name("新しいフォルダ").is_ok());
        assert!(validate_entry_name("  trimmed.txt  ").is_ok());
    }

    #[test]
    fn 空やドットのみの名前は拒否される() {
        assert!(validate_entry_name("").is_err());
        assert!(validate_entry_name("   ").is_err());
        assert!(validate_entry_name(".").is_err());
        assert!(validate_entry_name("..").is_err());
    }

    #[test]
    fn パス区切りや予約文字を含む名前は拒否される() {
        // パス脱出（区切り文字/..）を名前に混ぜられない（封じ込めの一次防御）。
        assert!(validate_entry_name("a/b.md").is_err());
        assert!(validate_entry_name("a\\b.md").is_err());
        assert!(validate_entry_name("..\\evil.md").is_err());
        // Windows 予約文字。
        assert!(validate_entry_name("a:b").is_err());
        assert!(validate_entry_name("a*b").is_err());
        assert!(validate_entry_name("a?b").is_err());
        assert!(validate_entry_name("a\"b").is_err());
        assert!(validate_entry_name("a<b").is_err());
        assert!(validate_entry_name("a>b").is_err());
        assert!(validate_entry_name("a|b").is_err());
        // 制御文字。
        assert!(validate_entry_name("a\u{0007}b").is_err());
    }

    #[test]
    fn 空の除外リストでは何も除外しない() {
        let empty: Vec<String> = Vec::new();
        assert!(!is_excluded_dir(".git", true, &empty));
        assert!(!is_excluded_dir("node_modules", true, &empty));
    }
}
