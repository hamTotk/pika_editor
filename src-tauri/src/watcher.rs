//! 監視スレッドの配線（要件7.1/7.4・design doc 4章/19章）。
//!
//! 役割（design doc 3章「固まらない」）:
//! - `notify` crate が供給する OS raw event を **pika-core::watcher の抽象型へ写す**だけ。
//! - 合成（デバウンス/合体・rename 正規化・自己保存抑制・オーバーフロー再同期）は
//!   **すべて pika-core::watcher**（UI 非依存・cargo test 済み）に委ねる。
//! - 合成結果を `emit('fs-changed', { changes })` でフロントへ送り、ツリー/タブの未読バッジを更新する。
//! - 監視スレッドは重い処理をせず raw event をキューに積む→定期 drain するだけ（UI を 200ms 超ブロックしない）。
//!
//! notify がオーバーフロー（ERROR_NOTIFY_ENUM_DIR 相当）を表面化するか／100件同時で取りこぼさないかは
//! 実機検証項目（系統C・design doc 15章-2）。本コードは notify の `Error` 種別を観測して
//! オーバーフロー扱い（全再列挙）へ落とすフックを置き、表面化しない場合は windows crate 直叩きへ
//! 切替える判断材料を findings に残す（acceptance-findings T-005）。

use notify::event::{CreateKind, EventKind, ModifyKind, RemoveKind, RenameMode};
use notify::{RecommendedWatcher, RecursiveMode, Watcher};
use pika_core::watcher::{
    is_pika_temp, Debouncer, FsChange, FsChangeKind, RawFsEvent, RawFsEventKind, SaveTokenStore,
    SuppressDecision,
};
use serde::Serialize;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{channel, Receiver};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant, UNIX_EPOCH};
use tauri::{AppHandle, Emitter};

use pika_core::watcher::overflow::{
    resync_against_baseline, BaselineEntry, FileFingerprint, ResyncOutcome,
};

/// 監視の既定除外（要件4.1・7.1。設定 `excluded_dirs` 未指定/空のときのフォールバック）。
///
/// ツリー列挙（commands.rs `enumerate_dir`）と同じ既定（`.git`/`node_modules`）。設定で `dist`/`target`
/// 等を足すと監視もそれに追従する（[`WatcherService::set_excluded_dirs`]）。設定が空のときは
/// 「除外ゼロ」にせず本既定へ倒し、`.git` 等の大量ノイズが復活して未読が消せなくなるのを防ぐ。
const DEFAULT_EXCLUDED_DIRS: &[&str] = &[".git", "node_modules"];

/// 既定除外の所有 Vec（`set_excluded_dirs` のフォールバック・`new` の初期値に使う）。
fn default_excluded_dirs() -> Vec<String> {
    DEFAULT_EXCLUDED_DIRS.iter().map(|s| s.to_string()).collect()
}

/// 与えられた除外リストの実効値を決める（空なら既定へフォールバック）。
///
/// 純粋関数として切り出し、`AppHandle` を要する [`WatcherService::set_excluded_dirs`] に頼らず
/// cargo test で「空→既定／指定→そのまま」を検証できるようにする。
fn effective_excluded(dirs: Vec<String>) -> Vec<String> {
    if dirs.is_empty() {
        default_excluded_dirs()
    } else {
        dirs
    }
}

/// 監視 drain の周期（デバウンス静穏期間を確実に拾える間隔）。
const DRAIN_INTERVAL: Duration = Duration::from_millis(50);

/// ポーリングフォールバックの既定間隔（要件7.1「既定5秒」）。
const POLL_INTERVAL: Duration = Duration::from_secs(5);

/// `emit('fs-changed')` のペイロード（フロントは changes を見て未読バッジを更新する）。
#[derive(Debug, Clone, Serialize)]
pub struct FsChangedPayload {
    pub changes: Vec<FsChangeDto>,
}

/// 合成結果 1 件の DTO（pika-core::FsChange を JSON 化）。
#[derive(Debug, Clone, Serialize)]
pub struct FsChangeDto {
    /// "created" | "modified" | "removed" | "renamed"
    pub kind: String,
    /// 対象パス（rename では新パス）。
    pub path: String,
    /// rename の旧パス（rename 以外は None）。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub from: Option<String>,
}

impl From<FsChange> for FsChangeDto {
    fn from(c: FsChange) -> Self {
        match c.kind {
            FsChangeKind::Created => FsChangeDto {
                kind: "created".into(),
                path: c.path,
                from: None,
            },
            FsChangeKind::Modified => FsChangeDto {
                kind: "modified".into(),
                path: c.path,
                from: None,
            },
            FsChangeKind::Removed => FsChangeDto {
                kind: "removed".into(),
                path: c.path,
                from: None,
            },
            FsChangeKind::Renamed { from } => FsChangeDto {
                kind: "renamed".into(),
                path: c.path,
                from: Some(from),
            },
        }
    }
}

/// 監視サービス。フロントの open_workspace でルートを設定し、保存時に自己保存トークンを登録する。
///
/// 監視方式（要件7.1）:
/// - `notify` が機能する FS は raw event 駆動。
/// - 機能しない FS（ネットワーク/UNC/クラウド）はポーリングフォールバックへ縮退（同じ再同期処理）。
/// - F5 はオンデマンドで全再列挙＋再同期（同じ処理を共有）。
pub struct WatcherService {
    app: AppHandle,
    inner: Arc<Mutex<WatcherInner>>,
}

struct WatcherInner {
    /// 現在の監視ルート（フォルダ or 単体ファイルの親）。
    root: Option<PathBuf>,
    /// 自己保存トークン台帳（保存層が register・合成層が消し込む）。
    save_tokens: SaveTokenStore,
    /// オーバーフロー再同期/ポーリング/F5 が突き合わせるベースライン台帳。
    baseline: BTreeMap<String, BaselineEntry>,
    /// ベースライン台帳の構築が完了したか（`watch_root` がワーカーで構築する間は false）。
    /// 未完了の間は F5/ポーリング/オーバーフロー再同期を見送る（空台帳と突き合わせると
    /// 全件 created の洪水になるため）。構築完了後にワーカーが true へ倒す。
    baseline_ready: bool,
    /// 保持中の notify watcher（drop で監視停止）。
    _watcher: Option<RecommendedWatcher>,
    /// ポーリングフォールバック中か（監視不能 FS。ステータス表示用）。
    polling: bool,
    /// 監視/列挙の除外ディレクトリ（settings.toml `excluded_dirs`・既定 `.git`/`node_modules`）。
    /// `open_workspace` が [`WatcherService::set_excluded_dirs`] で設定値を流し込む。常に実効値
    /// （空指定でも既定へフォールバック済み）を保持し、`is_excluded`/`walk` がワーカースレッドから読む。
    excluded_dirs: Vec<String>,
}

impl WatcherService {
    /// サービスを作る（監視は open_workspace まで開始しない）。
    pub fn new(app: AppHandle) -> Self {
        Self {
            app,
            inner: Arc::new(Mutex::new(WatcherInner {
                root: None,
                save_tokens: SaveTokenStore::new(),
                baseline: BTreeMap::new(),
                baseline_ready: false,
                _watcher: None,
                polling: false,
                excluded_dirs: default_excluded_dirs(),
            })),
        }
    }

    /// 監視/列挙の除外ディレクトリを設定する（settings.toml `excluded_dirs` を watcher へ配線）。
    ///
    /// `open_workspace` が `watch_root` の**前**に呼び、ツリー列挙（`enumerate_dir`）と同じ除外を
    /// 監視（baseline 列挙・notify イベント・ポーリング/F5/オーバーフロー再同期）にも効かせる。
    /// これまで watcher は `.git`/`node_modules` をハードコードしており、ユーザーが `dist`/`target` を
    /// 足すとツリーから消えるのに fs-changed が飛び続け未読が消せない不整合があった（指摘・解消）。
    /// 空 or 未設定時は既定 `[".git","node_modules"]` へフォールバックする（除外ゼロでノイズ復活を防ぐ）。
    /// 一致規則は `enumerate_dir` の `is_excluded_dir` と同じく大小無視（`is_excluded`）。
    /// ロック毒化時は握り潰す（既定のまま安全側に倒れる＝設定反映失敗で監視を止めない）。
    pub fn set_excluded_dirs(&self, dirs: Vec<String>) {
        let effective = effective_excluded(dirs);
        if let Ok(mut inner) = self.inner.lock() {
            inner.excluded_dirs = effective;
        }
    }

    /// フォルダ監視を開始する（既存監視は置換）。初回ベースラインも取得する。
    ///
    /// notify の購読開始に失敗した場合（一部 FS）はポーリングフォールバックへ縮退する（要件7.1）。
    pub fn watch_root(&self, root: &Path) -> Result<(), String> {
        // ベースライン（mtime/size 台帳）の構築はワーカースレッドへ逃がす（下の spawn_baseline_build）。
        // 巨大ツリー（例: target/ 配下が数万ファイル）でも command スレッド/ロックを 200ms 超
        // ブロックしない（設計原則2「固まらない」）。ここではロックを短く握って root を差し替え、
        // 旧 baseline を捨てて「未構築」へ倒すだけにする。
        let excluded = {
            let mut inner = self.inner.lock().map_err(|_| "watcher 状態ロック失敗")?;
            inner.root = Some(root.to_path_buf());
            // 旧ルートの台帳は破棄し、構築完了まで「未準備」にする。未準備の間は
            // F5/ポーリング/オーバーフロー再同期を空 baseline と突き合わせない（created 洪水を防ぐ）。
            inner.baseline = BTreeMap::new();
            inner.baseline_ready = false;
            inner.excluded_dirs.clone()
        };

        // notify watcher は同期で張る（baseline 構築を待たずにイベント取りこぼしを防ぐ）。
        // 通常の notify 経路（handle_notify_event→drain_and_emit）は baseline 有無に依存せず、
        // 自己保存抑制は save_tokens で行うため、構築中の外部変更も正しく検知・emit される。
        let (tx, rx) = channel();
        let watch_result =
            notify::recommended_watcher(move |res: Result<notify::Event, notify::Error>| {
                let _ = tx.send(res);
            })
            .and_then(|mut w| {
                w.watch(root, RecursiveMode::Recursive)?;
                Ok(w)
            });

        match watch_result {
            Ok(w) => {
                // ロック毒化時も watcher を取り落とさない（into_inner で回復・他箇所と同作法）。
                let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
                inner._watcher = Some(w);
                inner.polling = false;
                drop(inner);
                self.spawn_event_loop(rx);
            }
            Err(e) => {
                // 監視不能 FS はポーリングへ縮退（機能を落とさない・要件7.1/12.1）。
                let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
                inner._watcher = None;
                inner.polling = true;
                drop(inner);
                self.spawn_polling_loop(root.to_path_buf());
                let _ = self.app.emit(
                    "watch-mode",
                    format!("監視不能のためポーリング（5秒間隔）に切替: {e}"),
                );
            }
        }

        // ベースラインをバックグラウンドで構築（重い再帰列挙を UI スレッド/ロックから分離）。
        self.spawn_baseline_build(root.to_path_buf(), excluded);
        Ok(())
    }

    /// ベースライン台帳（mtime/size）をワーカースレッドで構築する。
    ///
    /// 巨大ツリーの再帰列挙を command スレッド/ロック外で行い「固まらない」を満たす。
    /// 構築完了までに `register_self_save` や notify 経路（`drain_and_emit`）が差し込んだ
    /// 最新エントリは `or_insert` で温存する（バックグラウンド結果で上書きしない）。
    /// 構築中に別フォルダを開き直した（root 差し替え）場合は結果を破棄する。
    fn spawn_baseline_build(&self, root: PathBuf, excluded: Vec<String>) {
        let inner = Arc::clone(&self.inner);
        thread::spawn(move || {
            // enumerate_baseline は mtime/size のみ採取（内容ハッシュは取らない）＝ファイルを読まない。
            let built = enumerate_baseline(&root, &excluded);
            let mut g = inner.lock().unwrap_or_else(|e| e.into_inner());
            // 構築中に別フォルダを開き直していたら破棄（古いルートの台帳で上書きしない）。
            if g.root.as_deref() != Some(root.as_path()) {
                return;
            }
            for (path, entry) in built {
                g.baseline.entry(path).or_insert(entry);
            }
            g.baseline_ready = true;
        });
    }

    /// 保存直前に自己保存トークンを登録する（保存後ハッシュ＋時刻）。
    /// watcher イベント側はハッシュ一致をもってワンショットで抑制する（要件7.1）。
    pub fn register_self_save(&self, path: &str, saved_hash: &str) {
        if let Ok(mut inner) = self.inner.lock() {
            inner
                .save_tokens
                .register(path, saved_hash, crate::util::now_ms());
            // 保存後はベースラインも自身の保存内容で更新（自己保存で未読を付けない）。
            // 変換は fingerprint_to_baseline_entry に集約し、content_hash だけ保存後ハッシュで上書きする
            // （fingerprint() は内容を読まず content_hash=None＝空のため、保存済みハッシュを明示で詰める）。
            if let Some(fp) = fingerprint(Path::new(path)) {
                let mut entry = fingerprint_to_baseline_entry(&fp);
                entry.content_hash = saved_hash.to_string();
                inner.baseline.insert(path.to_string(), entry);
            }
        }
    }

    /// F5（要件7.1/11.2）= オンデマンドの全再列挙＋再同期。ポーリング/オーバーフローと同じ
    /// [`resync_and_emit`] を共有する（3経路の重複と「ロック保持中の再帰列挙」を解消・固まらない原則）。
    pub fn resync_now(&self) -> Result<usize, String> {
        Ok(resync_and_emit(&self.inner, &self.app, Some("再同期中...")))
    }

    /// raw event を受けて合成し emit するスレッドを起動する。
    fn spawn_event_loop(&self, rx: Receiver<Result<notify::Event, notify::Error>>) {
        let app = self.app.clone();
        let inner = Arc::clone(&self.inner);
        thread::spawn(move || {
            let mut debouncer = Debouncer::new();
            // rename ペアは短い時間窓でバッファして正規化する。
            let mut rename_buf: Vec<RawFsEvent> = Vec::new();
            let mut last_drain = Instant::now();

            loop {
                // raw event を最大 DRAIN_INTERVAL 待って受ける（キューに積むだけ＝軽量）。
                match rx.recv_timeout(DRAIN_INTERVAL) {
                    Ok(Ok(event)) => {
                        // #37: notify の正式なオーバーフロー通知 API。need_rescan() が true なら
                        // 「取りこぼし（バッファ溢れ等）→全再列挙が必要」を意味する（notify 6.1）。
                        // 実オーバーフロー挙動は系統C実機検証が要る（acceptance T-005）。
                        if event.need_rescan() {
                            resync_and_emit(&inner, &app, Some("オーバーフロー再同期中..."));
                        } else {
                            handle_notify_event(
                                &event,
                                &inner,
                                &mut debouncer,
                                &mut rename_buf,
                                &app,
                            );
                        }
                    }
                    Ok(Err(e)) => {
                        // notify の Error 経路。オーバーフロー相当なら全再列挙で再同期する（保険）。
                        if is_overflow_error(&e) {
                            resync_and_emit(&inner, &app, Some("オーバーフロー再同期中..."));
                        }
                    }
                    Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
                    Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => break,
                }

                // 一定間隔で確定済みを drain して emit（rename も時間窓を過ぎたら確定）。
                if last_drain.elapsed() >= DRAIN_INTERVAL {
                    drain_and_emit(&inner, &mut debouncer, &mut rename_buf, &app);
                    last_drain = Instant::now();
                }
            }
        });
    }

    /// ポーリングフォールバックスレッドを起動する（監視不能 FS。要件7.1）。
    ///
    /// 5秒ごとに [`resync_and_emit`] で全再列挙＋再同期する（列挙はロック外＝固まらない原則）。
    /// 監視ルートが切替/停止されたらこのスレッドの担当 FS ではなくなるので break する
    /// （`resync_and_emit` は現在ルートを見て働くため、別ルートへ切替わった後も走り続けないよう終端する）。
    /// ステータス文言（watch-mode）はポーリングでは毎ティック出さない（`None`）。
    fn spawn_polling_loop(&self, root: PathBuf) {
        let app = self.app.clone();
        let inner = Arc::clone(&self.inner);
        thread::spawn(move || loop {
            thread::sleep(POLL_INTERVAL);
            // ルートが切替/停止されたら終了（このポーリングスレッドの担当 FS ではなくなった）。
            let still_root = match inner.lock() {
                Ok(g) => g.root.as_ref().map(|r| r == &root).unwrap_or(false),
                Err(_) => false,
            };
            if !still_root {
                break;
            }
            // ready 判定・列挙・比較・baseline 前進・emit は resync_and_emit に集約（3経路共通）。
            resync_and_emit(&inner, &app, None);
        });
    }
}

/// notify の 1 イベントを pika-core の RawFsEvent へ写し、合成層へ供給する。
fn handle_notify_event(
    event: &notify::Event,
    inner: &Arc<Mutex<WatcherInner>>,
    debouncer: &mut Debouncer,
    rename_buf: &mut Vec<RawFsEvent>,
    _app: &AppHandle,
) {
    let now = crate::util::now_ms();
    // 除外リストはワーカースレッドから Arc<Mutex> 経由で読む（set_excluded_dirs で更新されうる）。
    // 1イベントの先頭で1回だけ短く読み、以降の自己保存抑制ロックとは入れ子にしない（デッドロック回避）。
    let excluded = inner
        .lock()
        .map(|g| g.excluded_dirs.clone())
        .unwrap_or_else(|_| default_excluded_dirs());
    for path in &event.paths {
        if is_excluded(path, &excluded) {
            continue;
        }
        let path_str = path.to_string_lossy().to_string();
        // pika 自身のアトミック書込が作る一時ファイル（*.pika.tmp）は監視・合成・baseline から完全除外する
        // （要件7.1・修正1）。除外しないと一時ファイルの Create/Modify/Remove がツリー/未読を汚す。
        if is_pika_temp(&path_str) {
            continue;
        }
        let (kind, is_rename) = match &event.kind {
            EventKind::Create(
                CreateKind::Any | CreateKind::File | CreateKind::Folder | CreateKind::Other,
            ) => (RawFsEventKind::Created, false),
            EventKind::Modify(ModifyKind::Name(RenameMode::From)) => {
                (RawFsEventKind::RenamedFrom, true)
            }
            EventKind::Modify(ModifyKind::Name(RenameMode::To)) => {
                (RawFsEventKind::RenamedTo, true)
            }
            EventKind::Modify(_) => (RawFsEventKind::Modified, false),
            EventKind::Remove(
                RemoveKind::Any | RemoveKind::File | RemoveKind::Folder | RemoveKind::Other,
            ) => (RawFsEventKind::Removed, false),
            // notify がオーバーフローを EventKind で出す実装ではここに来ないが、保険として無視。
            _ => continue,
        };

        let meta = fingerprint(path);
        let raw = RawFsEvent {
            kind: kind.clone(),
            path: path_str.clone(),
            at_ms: now,
            // FileId は rename 正規化（段1）でしか使わない。Modified/Created の洪水で
            // CreateFileW を撒かないよう、rename イベントのときだけ採取する（#36 コスト削減）。
            file_id: if is_rename { file_id_of(path) } else { None },
            mtime_ms: meta.as_ref().map(|f| f.mtime_ms),
            size: meta.as_ref().map(|f| f.size),
        };

        if is_rename {
            rename_buf.push(raw);
            continue;
        }

        // 自己保存抑制はここ（イベント受信時）では行わず、合成後の単一関門 [`drain_and_emit`] で
        // 一括判定する（修正1）。アトミック置換保存は対象パスへ Removed/Modified/Created/rename-to の
        // **複数イベント**を撒くため、Modified|Created だけを受信時に抑制する旧方式では Removed/rename-to が
        // 素通りして取り消し線（未読 removed）を立てていた。デバウンス後の合成結果（種別合体済み）に対し
        // 現ディスク内容ハッシュ＝保存後ハッシュで抑制する方が、全種別を覆う堅い関門になる。
        debouncer.feed(&raw);
    }
}

/// 確定済みの変更を drain し、rename バッファを正規化して emit する。
fn drain_and_emit(
    inner: &Arc<Mutex<WatcherInner>>,
    debouncer: &mut Debouncer,
    rename_buf: &mut Vec<RawFsEvent>,
    app: &AppHandle,
) {
    let now = crate::util::now_ms();
    let mut changes: Vec<FsChange> = debouncer.drain_settled(now);

    // rename バッファに溜まったペアを正規化（時間窓を過ぎたものを確定）。
    if !rename_buf.is_empty() {
        let resolutions = pika_core::watcher::normalize_renames(rename_buf);
        rename_buf.clear();
        for r in resolutions {
            if let Some(c) = r.to_change() {
                changes.push(c);
            }
        }
    }

    // 自己保存抑制＋偽Removed除去＋トークンGC＋baseline前進を **1 つの critical section** で行う（修正1・#14）。
    // - 自己保存抑制（単一関門）: 各 change の対象 path に保存トークンがあり、現ディスク内容ハッシュが
    //   保存後ハッシュと一致するなら捨てる。Created/Modified だけでなく Removed/rename-to(Created) も覆う
    //   （アトミック置換が撒く複数の自己イベントを全部飲む＝取り消し線の根因を塞ぐ）。`hash_file` の I/O は
    //   トークンがある path に限って行い（`contains` で短絡）、ロック保持中の不要なファイル読みを避ける
    //   （#13 TOCTOU はハッシュ採取と decide を同一ロックに閉じ込めて解消・固まらない範囲）。
    // - 偽Removed: アトミック置換で実体が消えていない Removed（まだ存在する）は取り消し線にせず Modified に
    //   倒す（単発 stat のみ＝再帰列挙ではない）。
    // - GC: 窓超過の保存トークンを掃除する（decide はワンショット消費しないため）。
    let emit_changes: Vec<FsChange> = match inner.lock() {
        Ok(mut g) => {
            g.save_tokens.gc_expired(now);
            let mut kept: Vec<FsChange> = Vec::with_capacity(changes.len());
            for c in changes {
                // 自己保存抑制（トークンがある path のみハッシュ採取）。
                if g.save_tokens.contains(&c.path) {
                    let disk_hash = hash_file(Path::new(&c.path));
                    if matches!(
                        g.save_tokens.decide(&c.path, disk_hash.as_deref(), now),
                        SuppressDecision::Suppress
                    ) {
                        continue; // 自己保存イベント＝未読を付けない（emit しない）。
                    }
                }
                // 偽Removed の補正: 実体がまだ存在する Removed は内容変更に倒す（置換で消えていない）。
                let c = if matches!(c.kind, FsChangeKind::Removed)
                    && Path::new(&c.path).exists()
                {
                    FsChange::modified(c.path)
                } else {
                    c
                };
                // baseline 台帳を前進（自己保存以外の外部変更。再同期と整合・#14）。
                match &c.kind {
                    FsChangeKind::Removed => {
                        g.baseline.remove(&c.path);
                    }
                    FsChangeKind::Renamed { from } => {
                        if let Some(entry) = g.baseline.remove(from) {
                            g.baseline.insert(c.path.clone(), entry);
                        }
                    }
                    FsChangeKind::Modified | FsChangeKind::Created => {
                        // FsChange は mtime/size を持たないので stat のみで採取（全文 read しない）。
                        // content_hash 空でも次回 prescreen は mtime+size で短絡＝再 emit しない。
                        if let Some(fp) = fingerprint(Path::new(&c.path)) {
                            g.baseline
                                .insert(c.path.clone(), fingerprint_to_baseline_entry(&fp));
                        }
                    }
                }
                kept.push(c);
            }
            kept
        }
        // ロック毒化時は emit しない（安全側＝偽の未読を撒かない）。
        Err(_) => return,
    };

    if emit_changes.is_empty() {
        return;
    }

    let dto: Vec<FsChangeDto> = emit_changes.into_iter().map(Into::into).collect();
    let _ = app.emit("fs-changed", FsChangedPayload { changes: dto });
}

/// [`FileFingerprint`]（mtime/size/任意ハッシュ）を baseline 台帳の 1 エントリへ写す。
///
/// content_hash は fp 由来（未算出なら空文字＝次回 prescreen は mtime+size で短絡）。
/// baseline 前進（[`advance_baseline`]）・初回列挙（[`enumerate_baseline`]）・イベント経路の前進
/// （[`drain_and_emit`]）・自己保存（[`WatcherService::register_self_save`]）で同型だった手書き変換を
/// 1 か所へ集約する。`BaselineEntry` は pika-core 型のため orphan rule で `From` を実装できず自由関数。
fn fingerprint_to_baseline_entry(fp: &FileFingerprint) -> BaselineEntry {
    BaselineEntry {
        mtime_ms: fp.mtime_ms,
        size: fp.size,
        content_hash: fp.content_hash.clone().unwrap_or_default(),
    }
}

/// 再同期 outcome で baseline 台帳を現状へ前進させる（#14・二重 emit 防止）。
///
/// watcher の baseline は「変更検知の参照点（last seen）」であり、SnapshotService の
/// 差分/確認用 baseline とは別物なので、前進させても差分・確認済み・未読の永続（frontend 管理）には
/// 影響しない。前進させないと F5/ポーリング/オーバーフローが毎回同じ変更を再 emit する。
/// 次回 prescreen（mtime+size 一致）で短絡させるため content_hash は current 由来でよい（再ハッシュ不要）。
///
/// `WatcherInner` 全体ではなく **baseline マップだけ**を受け取る純粋関数にして cargo test で前進規則
/// （created/modified は current で上書き・removed は削除・current 不在は無視）を直接検証できるようにする。
fn advance_baseline(
    baseline: &mut BTreeMap<String, BaselineEntry>,
    current: &BTreeMap<String, FileFingerprint>,
    outcome: &ResyncOutcome,
) {
    for path in outcome.created.iter().chain(outcome.modified.iter()) {
        if let Some(fp) = current.get(path) {
            baseline.insert(path.clone(), fingerprint_to_baseline_entry(fp));
        }
    }
    for path in &outcome.removed {
        baseline.remove(path);
    }
}

/// resync の 3 経路（F5/ポーリング/オーバーフロー）共通の「全再列挙＋再同期＋emit」。
///
/// **固まらない原則（design doc 1章）**: 重い再帰列挙（[`enumerate_fingerprints`]）を必ず**ロック外**で
/// 行う。手順は『短いロックで (root, ready, excluded) を読む→未ルート/ベースライン未構築なら 0 で早期
/// return→（`status` 指定時）watch-mode を emit→**ロック外で列挙**→再ロックで **root 不変＋ready を再検証**
/// （列挙中の切替/停止なら 0 で早期 return）→baseline 比較＋前進（#14）→ロック解放後に changes 非空なら
/// fs-changed を emit→検知件数を返す』。
///
/// 旧実装では `resync_now` だけがロックを保持したまま列挙していた（固まらない原則違反）。3 経路を本関数へ
/// 統合してこれを解消し、ready 判定・emit ペイロード形・二重 emit 防止（baseline 前進）を 1 か所に集約する。
/// 列挙をロック外へ出した分、再ロック後の root/ready 再検証で「列挙中にフォルダを開き直したら別ルートの
/// baseline を汚染する」競合を塞ぐ（旧 `resync_now` の全期間ロック保持と同じ不変条件を回復）。
/// `status` は watch-mode のステータス文言（F5・オーバーフローは Some、ポーリングは毎ティック出さず None）。
fn resync_and_emit(
    inner: &Arc<Mutex<WatcherInner>>,
    app: &AppHandle,
    status: Option<&str>,
) -> usize {
    // 短いロックで root/excluded を読む（重い列挙はこの後ロック外で行う＝固まらない原則）。
    // 未ルート／ベースライン未構築中は再同期しない（空台帳と突き合わせると全件 created の洪水になる）。
    let (root, excluded) = match inner.lock() {
        Ok(g) => {
            let Some(root) = g.root.clone() else {
                return 0;
            };
            if !g.baseline_ready {
                return 0;
            }
            (root, g.excluded_dirs.clone())
        }
        Err(_) => return 0,
    };
    if let Some(msg) = status {
        let _ = app.emit("watch-mode", msg.to_string());
    }
    // enumerate はロック外（重い I/O）。比較と baseline 前進は再ロックして同一ロック内で行う（#14）。
    let current = enumerate_fingerprints(&root, &excluded);
    let (count, changes) = match inner.lock() {
        Ok(mut g) => {
            // 列挙中にフォルダ切替/停止が起きていないか再検証する。root が差し替わっている／
            // baseline が未準備に戻っている場合、別ルートで集めた current を現 baseline と突き合わせると
            // 偽の created/removed を撒き baseline を汚染する。旧 resync_now は列挙ごとロック保持でこの窓が
            // 無かった。再ロック後に root 不変＋ready を確認し、崩れていれば何もせず 0 を返す
            // （spawn_baseline_build の破棄判定と同じ作法）。
            if g.root.as_deref() != Some(root.as_path()) || !g.baseline_ready {
                return 0;
            }
            let outcome = resync_against_baseline(&g.baseline, &current);
            let count = outcome.total();
            advance_baseline(&mut g.baseline, &current, &outcome);
            let changes: Vec<FsChangeDto> =
                outcome.into_changes().into_iter().map(Into::into).collect();
            (count, changes)
        }
        Err(_) => return 0,
    };
    if !changes.is_empty() {
        let _ = app.emit("fs-changed", FsChangedPayload { changes });
    }
    count
}

/// notify の Error がオーバーフロー（ERROR_NOTIFY_ENUM_DIR 相当）か。
/// notify が表面化しない場合は常に false になり、その判断材料を findings に残す（系統C）。
fn is_overflow_error(e: &notify::Error) -> bool {
    matches!(e.kind, notify::ErrorKind::Generic(ref s) if s.contains("overflow"))
        || matches!(&e.kind, notify::ErrorKind::Io(io) if io.raw_os_error() == Some(0x3FF))
}

/// パスが除外配下か（`excluded` のいずれかを成分に含む・大小無視）。要件4.1/7.1。
///
/// 一致規則は `enumerate_dir` の `is_excluded_dir` と同じく `eq_ignore_ascii_case`（Windows のパス
/// 大小無視に揃える）。「パス成分のいずれかが一致」の意味論（配下まるごと除外）は従来どおり維持する。
fn is_excluded(path: &Path, excluded: &[String]) -> bool {
    path.components().any(|c| {
        c.as_os_str()
            .to_str()
            .map(|s| excluded.iter().any(|e| e.eq_ignore_ascii_case(s)))
            .unwrap_or(false)
    })
}

/// 監視ルート配下を再帰列挙してベースライン台帳を作る（初回オープン＝全既読スタート。要件8.1）。
///
/// 性能（設計原則2「固まらない」/3「軽い」）: ここでは **mtime/size のみ** を採取し、内容ハッシュは
/// 取らない（＝開く瞬間に全ファイルを読まない）。空 `content_hash` は `drain_and_emit`/`advance_baseline`
/// が入れるエントリと同じ扱いで、resync のプレスクリーン（mtime+size 一致→未変更で短絡）が機能する。
/// 内容ハッシュが要るのは「mtime/size が変わったのに内容が同じ」を抑制する場面だけで、その照合は
/// 変更検知時に当該ファイル 1 件だけ `hash_file` で行う（`handle_notify_event`）。
fn enumerate_baseline(root: &Path, excluded: &[String]) -> BTreeMap<String, BaselineEntry> {
    let mut map = BTreeMap::new();
    for (path, fp) in walk(root, excluded) {
        map.insert(path, fingerprint_to_baseline_entry(&fp));
    }
    map
}

/// 再同期用に現状の fingerprint を列挙する（プレスクリーン後のハッシュは比較段で算出）。
fn enumerate_fingerprints(root: &Path, excluded: &[String]) -> BTreeMap<String, FileFingerprint> {
    walk(root, excluded).into_iter().collect()
}

/// 再帰列挙の共通実装（除外を適用・ファイルのみ）。
///
/// FSエッジ（要件12.1・design doc 19章）:
/// - **シンボリックリンク循環検出**: 訪問済みディレクトリの canonical パスを集合で持ち、
///   再訪を打ち切る（リンクループで無限再帰しない）。
/// - **OneDrive プレースホルダ除外**: オンデマンド（未ダウンロード）ファイルは
///   ベースライン取得でダウンロードを誘発しないよう、reparse point 属性のファイルを
///   ベースライン対象から除外する（要件12.1。属性判定は `is_placeholder`）。
fn walk(root: &Path, excluded: &[String]) -> Vec<(String, FileFingerprint)> {
    let mut out = Vec::new();
    let mut visited: std::collections::HashSet<PathBuf> = std::collections::HashSet::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        // 循環検出: canonical 済みパスを訪問済みに記録し、再訪はスキップ。
        let canon = std::fs::canonicalize(&dir).unwrap_or_else(|_| dir.clone());
        if !visited.insert(canon) {
            continue;
        }
        let Ok(rd) = std::fs::read_dir(&dir) else {
            continue;
        };
        for ent in rd.flatten() {
            let p = ent.path();
            if is_excluded(&p, excluded) {
                continue;
            }
            match ent.file_type() {
                Ok(ft) if ft.is_dir() => stack.push(p),
                Ok(ft) if ft.is_file() => {
                    // OneDrive プレースホルダ（オンデマンド未取得）は除外（要件12.1）。
                    if is_placeholder(&p) {
                        continue;
                    }
                    let p_str = p.to_string_lossy().to_string();
                    // pika 自身の一時ファイル（*.pika.tmp）は baseline/再同期列挙から除外する（修正1・要件7.1）。
                    // 列挙に載せると保存中の一瞬で baseline へ入り、置換後に偽 Removed を生む。
                    if is_pika_temp(&p_str) {
                        continue;
                    }
                    if let Some(fp) = fingerprint(&p) {
                        out.push((p_str, fp));
                    }
                }
                _ => {}
            }
        }
    }
    out
}

/// OneDrive 等のオンデマンド未取得プレースホルダか（FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 等）。
/// 取得を誘発しないようベースライン対象から外す（要件12.1）。
#[cfg(windows)]
fn is_placeholder(path: &Path) -> bool {
    use std::os::windows::fs::MetadataExt;
    const FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS: u32 = 0x0040_0000;
    const FILE_ATTRIBUTE_RECALL_ON_OPEN: u32 = 0x0004_0000;
    const FILE_ATTRIBUTE_OFFLINE: u32 = 0x0000_1000;
    match std::fs::metadata(path) {
        Ok(meta) => {
            let attrs = meta.file_attributes();
            attrs
                & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
                    | FILE_ATTRIBUTE_RECALL_ON_OPEN
                    | FILE_ATTRIBUTE_OFFLINE)
                != 0
        }
        Err(_) => false,
    }
}

#[cfg(not(windows))]
fn is_placeholder(_path: &Path) -> bool {
    false
}

/// 1 ファイルの軽量 fingerprint（mtime/サイズのみ。ハッシュは比較段で必要時に算出）。
fn fingerprint(path: &Path) -> Option<FileFingerprint> {
    let meta = std::fs::metadata(path).ok()?;
    if !meta.is_file() {
        return None;
    }
    let mtime_ms = meta
        .modified()
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);
    Some(FileFingerprint {
        mtime_ms,
        size: meta.len(),
        content_hash: None,
    })
}

/// ファイル内容を LF 正規化してハッシュ化する（自己保存抑制・再同期の最終照合用）。
/// 改行のみの差を差分に出さない方針（要件8.1）と整合させるため LF 正規化してからハッシュする。
/// LF 正規化＋XxHash64 の本体は pika-core の単一ルーチンへ委譲（#40・出力完全一致）。
fn hash_file(path: &Path) -> Option<String> {
    let bytes = std::fs::read(path).ok()?;
    Some(pika_core::hashing::hash_normalized_lf(&bytes))
}

/// OS のファイル ID を採取する（rename 正規化の主キー。取得不能なら None）。
///
/// `GetFileInformationByHandle` でボリュームシリアル＋ファイルインデックスを採取する（#36）。
/// 安定版 std では file_index/volume_serial が未提供のため windows-sys を直叩きする。
/// **限界**: rename の From 側は呼び出し時点でファイルが既に消えており `CreateFileW` が失敗→None になる
/// （正常。From↔To のペア化は段2 のパス一意性が主役で、FileId は段1 のスワップ/上書き解決の補強）。
#[cfg(windows)]
fn file_id_of(path: &Path) -> Option<pika_core::watcher::FileId> {
    use windows_sys::Win32::Foundation::{CloseHandle, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Storage::FileSystem::{
        CreateFileW, GetFileInformationByHandle, BY_HANDLE_FILE_INFORMATION,
        FILE_FLAG_BACKUP_SEMANTICS, FILE_READ_ATTRIBUTES, FILE_SHARE_DELETE, FILE_SHARE_READ,
        FILE_SHARE_WRITE, OPEN_EXISTING,
    };

    // パスを NUL 終端 UTF-16 へ変換（Win32 境界・CLAUDE.md「Win32 境界で UTF-16 に変換」）。
    let wide = crate::util::to_wide(path);

    // SAFETY: wide は NUL 終端済みの有効な UTF-16 バッファ。属性読取のみ・全共有モードで開く
    // （他プロセスのアクセスを妨げない）。FILE_FLAG_BACKUP_SEMANTICS でディレクトリも開ける。
    // 不在・アクセス不可なら INVALID_HANDLE_VALUE が返り、その場合は None を返す。
    let handle = unsafe {
        CreateFileW(
            wide.as_ptr(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            std::ptr::null(),
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            std::ptr::null_mut(),
        )
    };
    if handle == INVALID_HANDLE_VALUE || handle.is_null() {
        return None;
    }

    // SAFETY: info は GetFileInformationByHandle が全フィールドを埋めるため zeroed 初期化で足りる。
    let mut info: BY_HANDLE_FILE_INFORMATION = unsafe { std::mem::zeroed() };
    // SAFETY: handle は CreateFileW が返した有効なハンドル。info は書き込み可能な領域。
    let ok = unsafe { GetFileInformationByHandle(handle, &mut info) };
    // SAFETY: handle は有効。早期 return でも漏らさないようここで必ず閉じる。
    unsafe {
        CloseHandle(handle);
    }
    if ok == 0 {
        return None;
    }

    let index = ((info.nFileIndexHigh as u64) << 32) | (info.nFileIndexLow as u64);
    Some(pika_core::watcher::FileId {
        volume: info.dwVolumeSerialNumber as u64,
        index,
    })
}

#[cfg(not(windows))]
fn file_id_of(_path: &Path) -> Option<pika_core::watcher::FileId> {
    None
}

// 合成層の時間窓判定に使う現在時刻（ミリ秒）は src-tauri 共通の [`crate::util::now_ms`] へ集約した
// （snapshot.rs と単一実装を共有。素朴版→クロック後退に耐える単調版へ格上げ＝堅牢化）。

#[cfg(test)]
mod tests {
    use super::*;

    fn dirs(v: &[&str]) -> Vec<String> {
        v.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn 実効除外_空は既定へフォールバックする() {
        // 設定 excluded_dirs が空（未指定/全削除）でも除外ゼロにせず既定（.git/node_modules）へ倒す。
        assert_eq!(effective_excluded(Vec::new()), default_excluded_dirs());
        assert_eq!(
            effective_excluded(Vec::new()),
            dirs(&[".git", "node_modules"])
        );
    }

    #[test]
    fn 実効除外_指定があればそのまま使う() {
        // ユーザーが dist/target を足したらそれをそのまま使う（既定は混ぜない＝設定で置換）。
        assert_eq!(
            effective_excluded(dirs(&["dist", "target"])),
            dirs(&["dist", "target"])
        );
    }

    #[test]
    fn 除外判定_既定の隠しディレクトリを成分に含むと除外する() {
        let ex = default_excluded_dirs();
        assert!(is_excluded(Path::new("/ws/.git/HEAD"), &ex));
        assert!(is_excluded(Path::new("/ws/node_modules/pkg/index.js"), &ex));
        // 除外配下でない通常ファイルは除外しない。
        assert!(!is_excluded(Path::new("/ws/src/main.ts"), &ex));
    }

    #[test]
    fn 除外判定_設定で足したディレクトリも除外する() {
        // 指摘の本丸: dist/target を設定で足すと監視/列挙からも消える（ツリーと一致＝未読が消せる）。
        let ex = dirs(&["dist", "target"]);
        assert!(is_excluded(Path::new("/ws/dist/bundle.js"), &ex));
        assert!(is_excluded(Path::new("/ws/target/debug/app.exe"), &ex));
        // 既定の .git は「設定で置換」されたので、この設定では除外されない（実効値＝指定そのまま）。
        assert!(!is_excluded(Path::new("/ws/.git/HEAD"), &ex));
    }

    #[test]
    fn 除外判定_大小無視で一致する() {
        // enumerate_dir の is_excluded_dir と同じく eq_ignore_ascii_case（Windows のパス大小無視）。
        let ex = dirs(&["Dist"]);
        assert!(is_excluded(Path::new("/ws/dist/x"), &ex));
        assert!(is_excluded(Path::new("/ws/DIST/x"), &ex));
        let ex_git = default_excluded_dirs();
        assert!(is_excluded(Path::new("/ws/.GIT/HEAD"), &ex_git));
    }

    fn fp(mtime: u64, size: u64, hash: Option<&str>) -> FileFingerprint {
        FileFingerprint {
            mtime_ms: mtime,
            size,
            content_hash: hash.map(|s| s.to_string()),
        }
    }

    #[test]
    fn fingerprint変換_ハッシュ無しは空文字になる() {
        // fingerprint() は内容を読まず content_hash=None を返す＝変換結果は空文字（prescreen は
        // mtime+size で短絡する）。drain_and_emit/enumerate_baseline と同じ扱いを保証する。
        assert_eq!(
            fingerprint_to_baseline_entry(&fp(100, 20, None)),
            BaselineEntry {
                mtime_ms: 100,
                size: 20,
                content_hash: String::new(),
            }
        );
    }

    #[test]
    fn fingerprint変換_ハッシュ有りはそのまま写す() {
        assert_eq!(
            fingerprint_to_baseline_entry(&fp(7, 8, Some("abc"))),
            BaselineEntry {
                mtime_ms: 7,
                size: 8,
                content_hash: "abc".to_string(),
            }
        );
    }

    #[test]
    fn baseline前進_新規と変更を現状で上書きし削除を消す() {
        // #14: created/modified は current の mtime/size/hash で上書き、removed は削除する。
        let mut baseline: BTreeMap<String, BaselineEntry> = BTreeMap::new();
        baseline.insert(
            "gone.md".into(),
            BaselineEntry {
                mtime_ms: 1,
                size: 1,
                content_hash: "g".into(),
            },
        );
        baseline.insert(
            "chg.md".into(),
            BaselineEntry {
                mtime_ms: 1,
                size: 1,
                content_hash: "old".into(),
            },
        );

        let mut current: BTreeMap<String, FileFingerprint> = BTreeMap::new();
        current.insert("chg.md".into(), fp(2, 2, Some("new")));
        current.insert("new.md".into(), fp(3, 3, None));

        let outcome = ResyncOutcome {
            modified: vec!["chg.md".into()],
            created: vec!["new.md".into()],
            removed: vec!["gone.md".into()],
        };

        advance_baseline(&mut baseline, &current, &outcome);

        // 削除は消える。
        assert!(!baseline.contains_key("gone.md"));
        // 変更は current の mtime/size/hash で上書き。
        assert_eq!(
            baseline.get("chg.md").unwrap(),
            &BaselineEntry {
                mtime_ms: 2,
                size: 2,
                content_hash: "new".into(),
            }
        );
        // 新規は current から追加（ハッシュ無し→空文字）。
        assert_eq!(
            baseline.get("new.md").unwrap(),
            &BaselineEntry {
                mtime_ms: 3,
                size: 3,
                content_hash: String::new(),
            }
        );
    }

    #[test]
    fn baseline前進_currentに無いcreated_modifiedは追加しない() {
        // outcome に載っていても current に指紋が無ければ baseline へ入れない（防御的・取得失敗時）。
        let mut baseline: BTreeMap<String, BaselineEntry> = BTreeMap::new();
        let current: BTreeMap<String, FileFingerprint> = BTreeMap::new();
        let outcome = ResyncOutcome {
            modified: vec!["ghost-mod.md".into()],
            created: vec!["ghost-new.md".into()],
            removed: Vec::new(),
        };
        advance_baseline(&mut baseline, &current, &outcome);
        assert!(baseline.is_empty());
    }
}
