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
    Debouncer, FsChange, FsChangeKind, RawFsEvent, RawFsEventKind, SaveTokenStore,
};
use serde::Serialize;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{channel, Receiver};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tauri::{AppHandle, Emitter};

use pika_core::watcher::overflow::{
    resync_against_baseline, BaselineEntry, FileFingerprint, ResyncOutcome,
};

/// 監視ルート直下の既定除外（要件4.1・7.1。監視対象外かつツリー非表示）。
const EXCLUDED_DIRS: &[&str] = &[".git", "node_modules"];

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
    /// 保持中の notify watcher（drop で監視停止）。
    _watcher: Option<RecommendedWatcher>,
    /// ポーリングフォールバック中か（監視不能 FS。ステータス表示用）。
    polling: bool,
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
                _watcher: None,
                polling: false,
            })),
        }
    }

    /// フォルダ監視を開始する（既存監視は置換）。初回ベースラインも取得する。
    ///
    /// notify の購読開始に失敗した場合（一部 FS）はポーリングフォールバックへ縮退する（要件7.1）。
    pub fn watch_root(&self, root: &Path) -> Result<(), String> {
        let mut inner = self.inner.lock().map_err(|_| "watcher 状態ロック失敗")?;
        inner.root = Some(root.to_path_buf());
        inner.baseline = enumerate_baseline(root);

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
                inner._watcher = Some(w);
                inner.polling = false;
                drop(inner);
                self.spawn_event_loop(rx);
                Ok(())
            }
            Err(e) => {
                // 監視不能 FS はポーリングへ縮退（機能を落とさない・要件7.1/12.1）。
                inner._watcher = None;
                inner.polling = true;
                let root = root.to_path_buf();
                drop(inner);
                self.spawn_polling_loop(root);
                let _ = self.app.emit(
                    "watch-mode",
                    format!("監視不能のためポーリング（5秒間隔）に切替: {e}"),
                );
                Ok(())
            }
        }
    }

    /// 保存直前に自己保存トークンを登録する（保存後ハッシュ＋時刻）。
    /// watcher イベント側はハッシュ一致をもってワンショットで抑制する（要件7.1）。
    pub fn register_self_save(&self, path: &str, saved_hash: &str) {
        if let Ok(mut inner) = self.inner.lock() {
            inner.save_tokens.register(path, saved_hash, now_ms());
            // 保存後はベースラインも自身の保存内容で更新（自己保存で未読を付けない）。
            if let Some(fp) = fingerprint(Path::new(path)) {
                inner.baseline.insert(
                    path.to_string(),
                    BaselineEntry {
                        mtime_ms: fp.mtime_ms,
                        size: fp.size,
                        content_hash: saved_hash.to_string(),
                    },
                );
            }
        }
    }

    /// F5（要件7.1/11.2）= オンデマンドの全再列挙＋再同期。ポーリングと同じ処理を共有する。
    pub fn resync_now(&self) -> Result<usize, String> {
        let mut inner = self.inner.lock().map_err(|_| "watcher 状態ロック失敗")?;
        let Some(root) = inner.root.clone() else {
            return Ok(0);
        };
        let _ = self.app.emit("watch-mode", "再同期中...".to_string());
        let current = enumerate_fingerprints(&root);
        let outcome = resync_against_baseline(&inner.baseline, &current);
        let count = outcome.total();
        // #14: baseline を現状へ前進させる（前進させないと F5 が毎回同じ変更を再 emit する）。
        // 次回 prescreen（mtime+size 一致）で短絡＝再 emit 停止。差分/確認用 baseline とは別物。
        advance_baseline(&mut inner, &current, &outcome);
        let changes: Vec<FsChangeDto> =
            outcome.into_changes().into_iter().map(Into::into).collect();
        drop(inner);
        if !changes.is_empty() {
            let _ = self.app.emit("fs-changed", FsChangedPayload { changes });
        }
        Ok(count)
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
                            do_overflow_resync(&inner, &app);
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
                            do_overflow_resync(&inner, &app);
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
    fn spawn_polling_loop(&self, root: PathBuf) {
        let app = self.app.clone();
        let inner = Arc::clone(&self.inner);
        thread::spawn(move || loop {
            thread::sleep(POLL_INTERVAL);
            // ルートが切替/停止されたら終了。
            let still_root = inner
                .lock()
                .ok()
                .and_then(|g| g.root.clone())
                .map(|r| r == root)
                .unwrap_or(false);
            if !still_root {
                break;
            }
            // enumerate はロック外（重い I/O）。比較と baseline 前進は同一ロック内で行う（#14）。
            let current = enumerate_fingerprints(&root);
            let (count, dto) = match inner.lock() {
                Ok(mut g) => {
                    let outcome = resync_against_baseline(&g.baseline, &current);
                    let count = outcome.total();
                    advance_baseline(&mut g, &current, &outcome);
                    let dto: Vec<FsChangeDto> =
                        outcome.into_changes().into_iter().map(Into::into).collect();
                    (count, dto)
                }
                Err(_) => continue,
            };
            if count > 0 {
                let _ = app.emit("fs-changed", FsChangedPayload { changes: dto });
            }
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
    let now = now_ms();
    for path in &event.paths {
        if is_excluded(path) {
            continue;
        }
        let path_str = path.to_string_lossy().to_string();
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

        // 自己保存抑制: 内容ハッシュ一致ならワンショットで抑制（未読を付けない）。
        // #13: ハッシュ採取と decide を 1 つの critical section に閉じ込め、ロック外で採った
        // stale なハッシュで判定する TOCTOU 窓を消す（hash_file は対象 1 ファイルの read のみで
        // ロック保持は短時間＝固まらない原則の範囲内）。
        if matches!(kind, RawFsEventKind::Modified | RawFsEventKind::Created) {
            if let Ok(mut g) = inner.lock() {
                let disk_hash = hash_file(path);
                if matches!(
                    g.save_tokens.decide(&path_str, disk_hash.as_deref(), now),
                    pika_core::watcher::SuppressDecision::Suppress
                ) {
                    continue;
                }
            }
        }
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
    let now = now_ms();
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

    if changes.is_empty() {
        return;
    }

    // ベースライン台帳を変更内容で前進させる（自己保存以外の外部変更。再同期と整合）。
    // #14: Modified/Created も前進させないと、イベント経路で報告済みの変更を F5/overflow が再検知する。
    if let Ok(mut g) = inner.lock() {
        for c in &changes {
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
                        g.baseline.insert(
                            c.path.clone(),
                            BaselineEntry {
                                mtime_ms: fp.mtime_ms,
                                size: fp.size,
                                content_hash: String::new(),
                            },
                        );
                    }
                }
            }
        }
    }

    let dto: Vec<FsChangeDto> = changes.into_iter().map(Into::into).collect();
    let _ = app.emit("fs-changed", FsChangedPayload { changes: dto });
}

/// 再同期 outcome で baseline 台帳を現状へ前進させる（#14・二重 emit 防止）。
///
/// watcher の baseline は「変更検知の参照点（last seen）」であり、SnapshotService の
/// 差分/確認用 baseline とは別物なので、前進させても差分・確認済み・未読の永続（frontend 管理）には
/// 影響しない。前進させないと F5/ポーリング/オーバーフローが毎回同じ変更を再 emit する。
/// 次回 prescreen（mtime+size 一致）で短絡させるため content_hash は current 由来でよい（再ハッシュ不要）。
fn advance_baseline(
    inner: &mut WatcherInner,
    current: &BTreeMap<String, FileFingerprint>,
    outcome: &ResyncOutcome,
) {
    for path in outcome.created.iter().chain(outcome.modified.iter()) {
        if let Some(fp) = current.get(path) {
            inner.baseline.insert(
                path.clone(),
                BaselineEntry {
                    mtime_ms: fp.mtime_ms,
                    size: fp.size,
                    content_hash: fp.content_hash.clone().unwrap_or_default(),
                },
            );
        }
    }
    for path in &outcome.removed {
        inner.baseline.remove(path);
    }
}

/// オーバーフロー検知時の全再列挙＋再同期（要件7.4・design doc 166行）。
fn do_overflow_resync(inner: &Arc<Mutex<WatcherInner>>, app: &AppHandle) {
    let root = inner.lock().ok().and_then(|g| g.root.clone());
    let Some(root) = root else { return };
    let _ = app.emit("watch-mode", "オーバーフロー再同期中...".to_string());
    // enumerate はロック外。比較と baseline 前進は再ロックして同一ロック内で行う（#14）。
    let current = enumerate_fingerprints(&root);
    let (count, dto) = match inner.lock() {
        Ok(mut g) => {
            let outcome = resync_against_baseline(&g.baseline, &current);
            let count = outcome.total();
            advance_baseline(&mut g, &current, &outcome);
            let dto: Vec<FsChangeDto> =
                outcome.into_changes().into_iter().map(Into::into).collect();
            (count, dto)
        }
        Err(_) => return,
    };
    if count > 0 {
        let _ = app.emit("fs-changed", FsChangedPayload { changes: dto });
    }
}

/// notify の Error がオーバーフロー（ERROR_NOTIFY_ENUM_DIR 相当）か。
/// notify が表面化しない場合は常に false になり、その判断材料を findings に残す（系統C）。
fn is_overflow_error(e: &notify::Error) -> bool {
    matches!(e.kind, notify::ErrorKind::Generic(ref s) if s.contains("overflow"))
        || matches!(&e.kind, notify::ErrorKind::Io(io) if io.raw_os_error() == Some(0x3FF))
}

/// パスが除外配下か（.git / node_modules を成分に含む）。要件4.1。
fn is_excluded(path: &Path) -> bool {
    path.components().any(|c| {
        c.as_os_str()
            .to_str()
            .map(|s| EXCLUDED_DIRS.contains(&s))
            .unwrap_or(false)
    })
}

/// 監視ルート直下を再帰列挙してベースライン台帳を作る（初回オープン＝全既読スタート。要件8.1）。
fn enumerate_baseline(root: &Path) -> BTreeMap<String, BaselineEntry> {
    let mut map = BTreeMap::new();
    for (path, fp) in walk(root) {
        let hash = fp
            .content_hash
            .clone()
            .unwrap_or_else(|| hash_file(&PathBuf::from(&path)).unwrap_or_default());
        map.insert(
            path,
            BaselineEntry {
                mtime_ms: fp.mtime_ms,
                size: fp.size,
                content_hash: hash,
            },
        );
    }
    map
}

/// 再同期用に現状の fingerprint を列挙する（プレスクリーン後のハッシュは比較段で算出）。
fn enumerate_fingerprints(root: &Path) -> BTreeMap<String, FileFingerprint> {
    walk(root).into_iter().collect()
}

/// 再帰列挙の共通実装（除外を適用・ファイルのみ）。
///
/// FSエッジ（要件12.1・design doc 19章）:
/// - **シンボリックリンク循環検出**: 訪問済みディレクトリの canonical パスを集合で持ち、
///   再訪を打ち切る（リンクループで無限再帰しない）。
/// - **OneDrive プレースホルダ除外**: オンデマンド（未ダウンロード）ファイルは
///   ベースライン取得でダウンロードを誘発しないよう、reparse point 属性のファイルを
///   ベースライン対象から除外する（要件12.1。属性判定は `is_placeholder`）。
fn walk(root: &Path) -> Vec<(String, FileFingerprint)> {
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
            if is_excluded(&p) {
                continue;
            }
            match ent.file_type() {
                Ok(ft) if ft.is_dir() => stack.push(p),
                Ok(ft) if ft.is_file() => {
                    // OneDrive プレースホルダ（オンデマンド未取得）は除外（要件12.1）。
                    if is_placeholder(&p) {
                        continue;
                    }
                    if let Some(fp) = fingerprint(&p) {
                        out.push((p.to_string_lossy().to_string(), fp));
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
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Foundation::{CloseHandle, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Storage::FileSystem::{
        CreateFileW, GetFileInformationByHandle, BY_HANDLE_FILE_INFORMATION,
        FILE_FLAG_BACKUP_SEMANTICS, FILE_READ_ATTRIBUTES, FILE_SHARE_DELETE, FILE_SHARE_READ,
        FILE_SHARE_WRITE, OPEN_EXISTING,
    };

    // パスを NUL 終端 UTF-16 へ変換（Win32 境界・CLAUDE.md「Win32 境界で UTF-16 に変換」）。
    let wide: Vec<u16> = path.as_os_str().encode_wide().chain(std::iter::once(0)).collect();

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

/// 単調増加に近い現在時刻（ミリ秒）。合成層の時間窓判定に使う。
fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}
