//! スナップショット/差分/確認済みの配線（要件8/9・design doc 11章）。
//!
//! 役割（design doc 3章「薄い境界」）:
//! - 差分計算・退避方針・確認済み判定・容量管理は **すべて pika-core**（cargo test 済み）に委ね、
//!   ここは「ベースライン内容 object の保持」と「FS 読取＋pika-core 呼び出し＋結果の DTO 化」に徹する。
//! - ベースライン内容は content-addressed object（メモリ保持＋将来はデータルート配下へ zstd 永続化）。
//!   本スプリントは中心体験③④（差分→確認済み）の貫通を優先し、object はメモリ保持で結線する
//!   （永続化・DACL は design doc 11章どおり後続で同じ pika-core 判定を再利用する）。
//!
//! 最上位原則「データを失わない」: 退避（object 保存）に失敗したらベースラインを進めず Result で返す。

use pika_core::diff::{compute_diff, DiffTag};
use pika_core::review::{
    decide_confirm, decide_rollback, ConfirmDecision, DiffSnapshot, DiskState,
};
use pika_core::snapshot::{
    baseline_policy, hash_normalized, BaselinePolicy, SnapshotStore, StashKind,
};
use serde::Serialize;
use std::collections::HashMap;
use std::path::Path;
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};
use tauri::State;

/// 差分 1 行の DTO（フロントの read-only unified レンダラと対応＝要件8.2）。
#[derive(Debug, Clone, Serialize)]
pub struct DiffLineDto {
    /// "equal" | "insert" | "delete"
    pub tag: String,
    /// 旧側行番号（追加行は None）。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub old_line_no: Option<usize>,
    /// 新側行番号（削除行は None）。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub new_line_no: Option<usize>,
    /// 行本文。
    pub content: String,
    /// 行内差分セグメント（変更語/grapheme を下線/太字で強調＝色非依存）。
    pub segments: Vec<SegmentDto>,
}

/// 行内差分セグメントの DTO。
#[derive(Debug, Clone, Serialize)]
pub struct SegmentDto {
    pub changed: bool,
    pub text: String,
}

/// ファイル差分の DTO（フロントへ返す）。
#[derive(Debug, Clone, Serialize)]
pub struct FileDiffDto {
    pub lines: Vec<DiffLineDto>,
    pub change_count: usize,
    /// 内容ベースラインを持つか（false なら差分・巻き戻し非対象＝ハッシュのみ）。
    pub has_baseline_content: bool,
}

/// スナップショットサービス（managed state）。ベースライン索引と内容 object を保持する。
pub struct SnapshotService {
    inner: Mutex<SnapshotInner>,
}

#[derive(Default)]
struct SnapshotInner {
    /// 索引（ベースライン/退避/参照計数・pika-core の決定論モデル）。
    store: SnapshotStore,
    /// content-addressed object（ハッシュ → LF 正規化済み内容）。
    /// design doc 11章では zstd 圧縮してデータルートへ永続化するが、本スプリントはメモリ保持で結線する。
    objects: HashMap<String, String>,
    /// 差分提示時点のディスクスナップショット（確認済み確定直前の再照合に使う＝要件8.3）。
    /// path → (mtime, hash, 提示した内容)。
    diff_snapshots: HashMap<String, DiffSnapshot>,
}

impl Default for SnapshotService {
    fn default() -> Self {
        Self {
            inner: Mutex::new(SnapshotInner::default()),
        }
    }
}

impl SnapshotService {
    pub fn new() -> Self {
        Self::default()
    }

    /// フォルダ初回オープン時にベースラインを取得する（全既読スタート＝要件8.1）。
    /// 内容保存方針（機密/10MB以上/画像＝ハッシュのみ）は pika-core::snapshot::policy で判定する。
    pub fn capture_baseline(&self, path: &str, content: &str, size: u64) {
        let mut inner = self.inner.lock().expect("snapshot lock");
        let key = normalize_path_key(path);
        let hash = hash_normalized(content);
        match baseline_policy(path, size) {
            BaselinePolicy::StoreContent => {
                let normalized = pika_core::diff::normalize_lf(content);
                inner.objects.insert(hash.clone(), normalized);
                inner
                    .store
                    .set_baseline_with_content(key, hash.clone(), hash);
            }
            BaselinePolicy::HashOnly => {
                inner.store.set_baseline_hash_only(key, hash);
            }
        }
    }
}

/// 索引キー用にパス区切りを正規化する（`\` → `/`）。
///
/// フロントの未読ストアは fs-changed のパスを `/` 区切りへ正規化して保持する（ui/unread.ts）。
/// 一方 open_workspace はネイティブ（`\`）区切りで baseline を張る。両者を同一キーで引けるよう
/// 索引アクセスは常に本関数でキーを揃える（FS 読取は元パスをそのまま使う＝区切り混在に強い）。
fn normalize_path_key(path: &str) -> String {
    path.replace('\\', "/")
}

/// ベースライン vs 現在内容の差分を計算する（要件8.2）。
///
/// タブで開いている場合は編集バッファ、開いていない場合はディスク内容を `current` に渡す。
/// ベースラインが内容を持たない（ハッシュのみ）場合は差分非対象として空の差分を返す。
#[tauri::command]
pub fn compute_file_diff(
    path: String,
    current: String,
    snapshot: State<'_, SnapshotService>,
) -> Result<FileDiffDto, String> {
    let mut inner = snapshot.inner.lock().map_err(|_| "snapshot ロック失敗")?;
    let key = normalize_path_key(&path);

    let Some(baseline) = inner.store.baseline(&key).cloned() else {
        // ベースライン未取得（新規＝全行追加）。
        let diff = compute_diff("", &current);
        return Ok(to_dto(diff, false));
    };

    let Some(object_hash) = baseline.object_hash.clone() else {
        // ハッシュのみ＝差分非対象（要件8.2/9.2）。
        return Ok(FileDiffDto {
            lines: Vec::new(),
            change_count: 0,
            has_baseline_content: false,
        });
    };

    let baseline_content = inner.objects.get(&object_hash).cloned().unwrap_or_default();
    let diff = compute_diff(&baseline_content, &current);

    // 差分提示時点のディスク状態を記録（確認済み確定直前の再照合に使う＝要件8.3）。
    let cur_hash = hash_normalized(&current);
    inner.diff_snapshots.insert(
        key,
        DiffSnapshot {
            mtime_ms: file_mtime_ms(&path),
            content_hash: cur_hash,
        },
    );
    inner
        .objects
        .entry(hash_normalized(&current))
        .or_insert_with(|| pika_core::diff::normalize_lf(&current));

    Ok(to_dto(diff, true))
}

/// 「確認済みにする」（要件8.3）。差分提示時点と現ディスクを再照合し、変化していなければ
/// ベースラインを差分時点内容へ更新する。変化していれば中断して再差分を促す。
#[tauri::command]
pub fn confirm_file(path: String, snapshot: State<'_, SnapshotService>) -> Result<bool, String> {
    let mut inner = snapshot.inner.lock().map_err(|_| "snapshot ロック失敗")?;
    let key = normalize_path_key(&path);

    let Some(frozen) = inner.diff_snapshots.get(&key).cloned() else {
        return Err("差分を表示してから確認済みにしてください".into());
    };

    // 確定直前にディスクを実読みして再照合（mtime 据え置きの取りこぼし対策にハッシュも見る）。
    let disk_content =
        std::fs::read_to_string(&path).map_err(|e| format!("ディスク再読みに失敗: {e}"))?;
    let disk = DiskState {
        mtime_ms: file_mtime_ms(&path),
        content_hash: hash_normalized(&disk_content),
    };
    let policy = baseline_policy(&path, disk_content.len() as u64);

    match decide_confirm(&frozen, &disk, policy) {
        ConfirmDecision::AbortReDiff => Ok(false), // 中断＝未読維持・再差分を促す。
        ConfirmDecision::UpdateBaseline {
            content_hash,
            store_content,
        } => {
            if store_content {
                let normalized = pika_core::diff::normalize_lf(&disk_content);
                inner.objects.insert(content_hash.clone(), normalized);
                inner
                    .store
                    .set_baseline_with_content(&key, content_hash.clone(), content_hash);
            } else {
                inner.store.set_baseline_hash_only(&key, content_hash);
            }
            inner.diff_snapshots.remove(&key);
            Ok(true)
        }
    }
}

/// 「確認済み時点に戻す」（ファイル単位巻き戻し・要件8.3・7.3 退避不能ガード）。
///
/// 退避不能（ベースラインがハッシュのみ／現在内容が10MB以上・画像）はブロックする。
/// 許可される場合は現在内容を rollback 退避してからベースライン内容を返す（呼び出し側が上書き）。
#[tauri::command]
pub fn rollback_file(path: String, snapshot: State<'_, SnapshotService>) -> Result<String, String> {
    let mut inner = snapshot.inner.lock().map_err(|_| "snapshot ロック失敗")?;
    let key = normalize_path_key(&path);

    let Some(baseline) = inner.store.baseline(&key).cloned() else {
        return Err("ベースラインがありません".into());
    };
    let Some(object_hash) = baseline.object_hash.clone() else {
        return Err("内容ベースラインが無いため巻き戻せません（10MB以上/画像）".into());
    };

    let disk_content =
        std::fs::read_to_string(&path).map_err(|e| format!("ディスク再読みに失敗: {e}"))?;
    let current_storable = baseline_policy(&path, disk_content.len() as u64).stores_content();

    // pika-core の退避不能ガードで判定（要件7.3）。
    decide_rollback(baseline.has_content(), current_storable).map_err(|e| e.to_string())?;

    // 現在内容を rollback 退避（退避が最後の砦＝確認ダイアログより退避が先）。
    let cur_hash = hash_normalized(&disk_content);
    let normalized = pika_core::diff::normalize_lf(&disk_content);
    inner.objects.insert(cur_hash.clone(), normalized);
    let stash = inner
        .store
        .add_stash(&key, StashKind::Rollback, cur_hash, now_ms(), true);
    // 退避が索引へ入ったことを確認（失敗時は add_stash が evict を返すのみで本体は入る）。
    debug_assert_eq!(stash.stashed_object, hash_normalized(&disk_content));

    // ベースライン内容を返す（呼び出し側がディスク/バッファへ上書きする）。
    let baseline_content = inner.objects.get(&object_hash).cloned().unwrap_or_default();
    Ok(baseline_content)
}

/// 「すべて確認済みにする」の結果 DTO（要件8.3）。
#[derive(Debug, Clone, Serialize)]
pub struct ConfirmAllResult {
    /// 確認済みにした件数。
    pub updated: usize,
    /// 処理中に変化したためスキップした件数（未読のまま残る）。
    pub skipped: usize,
    /// baseline-replace バッチへ退避した更新前 object 件数（一括取り消しの対象）。
    pub stashed: usize,
}

/// 「すべて確認済みにする」（要件8.3）。
///
/// `paths` は実行開始時点でフロントがフリーズした未読集合。各ファイルについて
/// 確定直前にディスクを再読みして再照合し、変化していないものだけベースラインを更新する。
/// 更新前ベースライン object は baseline-replace バッチへ一括退避する（ワンクリック一括取り消し可能）。
#[tauri::command]
pub fn confirm_all(
    paths: Vec<String>,
    snapshot: State<'_, SnapshotService>,
) -> Result<ConfirmAllResult, String> {
    use pika_core::review::{decide_confirm_all, ConfirmAllOutcome, ConfirmAllTarget};

    let mut inner = snapshot.inner.lock().map_err(|_| "snapshot ロック失敗")?;

    // 実行開始時点の未読集合をフリーズしてターゲットを組み立てる（要件8.3）。
    let now = now_ms();
    let mut targets: Vec<ConfirmAllTarget> = Vec::new();
    // (path, disk_content) を退避/object 保存用に保持する。
    let mut disk_map: HashMap<String, String> = HashMap::new();
    for path in &paths {
        let key = normalize_path_key(path);
        let frozen = match inner.diff_snapshots.get(&key).cloned() {
            Some(f) => f,
            None => continue, // 差分未提示のものは対象外（見ていない内容を確定しない）。
        };
        let disk_content = std::fs::read_to_string(path).unwrap_or_default();
        let disk = DiskState {
            mtime_ms: file_mtime_ms(path),
            content_hash: hash_normalized(&disk_content),
        };
        let policy = baseline_policy(path, disk_content.len() as u64);
        let prev_baseline_object = inner
            .store
            .baseline(&key)
            .and_then(|b| b.object_hash.clone());
        targets.push(ConfirmAllTarget {
            // rel_path はキー（正規化済み）にして以後の索引アクセスと揃える。
            rel_path: key.clone(),
            frozen,
            disk,
            prev_baseline_object,
            policy,
        });
        disk_map.insert(key, disk_content);
    }

    let outcomes = decide_confirm_all(&targets);
    let mut result = ConfirmAllResult {
        updated: 0,
        skipped: 0,
        stashed: 0,
    };

    for outcome in outcomes {
        match outcome {
            ConfirmAllOutcome::SkippedChanged { .. } => result.skipped += 1,
            ConfirmAllOutcome::Updated {
                rel_path,
                content_hash,
                stash_object,
                store_content,
            } => {
                // 更新前ベースライン object を baseline-replace バッチへ退避（10件枠とは別＝要件8.3）。
                if let Some(obj) = stash_object {
                    inner.store.add_stash(
                        &rel_path,
                        StashKind::BaselineReplace,
                        obj,
                        now,
                        false, // baseline-replace は LRU 枠を消費しない。
                    );
                    result.stashed += 1;
                }
                // ベースラインを差分時点（フリーズ内容＝現ディスク）へ更新。
                if store_content {
                    if let Some(disk) = disk_map.get(&rel_path) {
                        let normalized = pika_core::diff::normalize_lf(disk);
                        inner.objects.insert(content_hash.clone(), normalized);
                    }
                    inner.store.set_baseline_with_content(
                        &rel_path,
                        content_hash.clone(),
                        content_hash,
                    );
                } else {
                    inner.store.set_baseline_hash_only(&rel_path, content_hash);
                }
                inner.diff_snapshots.remove(&rel_path);
                result.updated += 1;
            }
        }
    }

    Ok(result)
}

/// FileDiff を DTO へ写す。
fn to_dto(diff: pika_core::diff::FileDiff, has_baseline_content: bool) -> FileDiffDto {
    FileDiffDto {
        lines: diff
            .lines
            .into_iter()
            .map(|l| DiffLineDto {
                tag: match l.tag {
                    DiffTag::Equal => "equal",
                    DiffTag::Insert => "insert",
                    DiffTag::Delete => "delete",
                }
                .to_string(),
                old_line_no: l.old_line_no,
                new_line_no: l.new_line_no,
                content: l.content,
                segments: l
                    .segments
                    .into_iter()
                    .map(|s| SegmentDto {
                        changed: s.changed,
                        text: s.text,
                    })
                    .collect(),
            })
            .collect(),
        change_count: diff.change_count,
        has_baseline_content,
    }
}

/// ファイルの mtime（ミリ秒）。取得不能は 0。
fn file_mtime_ms(path: &str) -> u64 {
    std::fs::metadata(Path::new(path))
        .ok()
        .and_then(|m| m.modified().ok())
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// 現在時刻（ミリ秒）。退避の created_at に使う。
fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}
