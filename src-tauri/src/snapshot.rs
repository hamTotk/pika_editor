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
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
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
    /// `snap_dir` が `Some` のとき、ここはデータルート配下 `objects/<hash>.zst` の **インメモリキャッシュ**
    /// （遅延ロードで満たす）。`None` のときは従来どおりメモリ保持のみ（後方互換）。
    objects: HashMap<String, String>,
    /// 差分提示時点のディスクスナップショット（確認済み確定直前の再照合に使う＝要件8.3）。
    /// path → (mtime, hash, 提示した内容)。
    diff_snapshots: HashMap<String, DiffSnapshot>,
    /// 永続スナップショット領域（`<data_root>/snapshots/<wsハッシュ>`）。
    /// `None` の間は永続化しない（ワークスペース未設定／data_root 解決失敗時の後方互換＝#3）。
    /// `Some` のとき各 mutation で object/メタ/index.json を書き、起動時にロードする（再起動で揮発しない）。
    snap_dir: Option<PathBuf>,
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

    /// ワークスペースの永続スナップショット領域を確定し、既存の索引/退避をロードする（#3）。
    ///
    /// 起動（フォルダオープン）時に1回呼ぶ。これにより「再起動で退避が揮発する」欠陥を解消する
    /// （最上位原則「データを失わない／退避が最後の砦」を再起動を跨いで成立させる）。
    ///
    /// 処理:
    /// 1. データルート最上位に DACL を張る（所有者＋SYSTEM 限定・失敗は警告ログのみで致命にしない）。
    /// 2. `<data_root>/snapshots/<wsハッシュ>/objects` を作る。
    /// 3. index.json をロード（正常／破損復元／初回空の3分岐）。破損時は object の自己記述メタから
    ///    退避一覧を再生成する（recover_stashes_from_meta＝最後の砦に到達）。
    /// 4. `snap_dir` を保持し、以後の各 mutation が object/メタ/index を永続化する。
    ///
    /// **objects は遅延ロード**（全件は読まない＝起動を重くしない）。内容を引く箇所
    /// （compute_file_diff/rollback_file の baseline 取得）で map ミス時に snap_dir から読む。
    pub fn set_workspace(&self, ws_root: &str, data_root: &Path) -> Result<(), String> {
        // 1. データルート最上位の DACL（失敗は致命にせず警告ログ・起動継続）。
        if let Err(e) = crate::snapshot_persist::harden_data_root_dacl(data_root) {
            crate::diagnostic::record(
                pika_core::diagnostic::LogLevel::Warn,
                "snapshot",
                "dacl",
                None,
                &e,
            );
        }

        // 2. 永続領域を確定（objects まで作る）。
        let snap_dir = crate::snapshot_persist::snapshot_dir_for(data_root, ws_root)?;

        // 3. index.json をロード（正常/破損復元/初回空）。
        let outcome = crate::snapshot_persist::load_store(&snap_dir);
        if matches!(
            outcome,
            crate::snapshot_persist::LoadOutcome::RecoveredFromMeta(_)
        ) {
            // index 破損→object の自己記述メタから退避を再生成した（最後の砦・要件9.1）。
            crate::diagnostic::record(
                pika_core::diagnostic::LogLevel::Warn,
                "snapshot",
                "recover",
                None,
                "index.json が読めず退避メタから退避一覧を復元しました",
            );
        }
        let store = outcome.into_store();

        // 4. inner を差し替える（store をロード結果へ・snap_dir を保持）。
        let mut inner = self.inner.lock().map_err(|_| "snapshot ロック失敗")?;
        inner.store = store;
        inner.objects.clear();
        inner.diff_snapshots.clear();
        inner.snap_dir = Some(snap_dir);
        Ok(())
    }

    /// フォルダ初回オープン時にベースラインを取得する（全既読スタート＝要件8.1）。
    /// 内容保存方針（機密/10MB以上/画像＝ハッシュのみ）は pika-core::snapshot::policy で判定する。
    ///
    /// **非クロバー化（#3）**: 既にベースラインがある path はスキップする。再オープン時に
    /// 永続済みベースライン（前回確認済み内容）を「全既読」で上書きしてしまうと、閉じている間の
    /// 外部変更が差分として残らない。要件8.1「全既読スタート」は **初回（ベースライン未保持）の
    /// ファイルにのみ**適用するのが正しい挙動。
    pub fn capture_baseline(&self, path: &str, content: &str, size: u64) {
        let mut inner = self.inner.lock().expect("snapshot lock");
        let key = normalize_path_key(path);
        // 既にベースラインがある（前回オープンで永続化された）path は触らない（非クロバー）。
        if inner.store.baseline(&key).is_some() {
            return;
        }
        let hash = hash_normalized(content);
        match baseline_policy(path, size) {
            BaselinePolicy::StoreContent => {
                let normalized = pika_core::diff::normalize_lf(content);
                inner.objects.insert(hash.clone(), normalized.clone());
                inner
                    .store
                    .set_baseline_with_content(&key, hash.clone(), hash.clone());
                // 内容 object を永続化（再起動でベースライン内容＝差分の素を失わない）。
                if let Some(dir) = inner.snap_dir.clone() {
                    crate::snapshot_persist::persist_object(&dir, &hash, &normalized);
                }
            }
            BaselinePolicy::HashOnly => {
                inner.store.set_baseline_hash_only(&key, hash);
            }
        }
        persist_index_locked(&mut inner);
    }

    /// フォルダ初回オープン時にハッシュのみベースラインを取得する（バイナリ等・#54）。
    ///
    /// バイナリ（非UTF-8）は内容 object を持てない（差分/巻き戻し非対象）が、外部変更検知のため
    /// バイト由来ハッシュをベースラインに据える（空文字ベースライン化＝全ファイル空ハッシュ衝突を避ける）。
    /// [`capture_baseline`] と同じく **非クロバー**（既存ベースラインがあれば触らない）で、
    /// mutation 完了直前に index を永続化する（バッチ8 の永続化機構を壊さない）。
    pub fn capture_baseline_hash_only(&self, path: &str, content_hash: &str) {
        let mut inner = self.inner.lock().expect("snapshot lock");
        let key = normalize_path_key(path);
        // 既にベースラインがある（前回オープンで永続化された）path は触らない（非クロバー）。
        if inner.store.baseline(&key).is_some() {
            return;
        }
        // 内容 object は持たない（HashOnly）。ハッシュのみを索引へ据える。
        inner
            .store
            .set_baseline_hash_only(&key, content_hash.to_string());
        persist_index_locked(&mut inner);
    }

    /// 破壊的上書き保存の**前に**、ディスク上の現内容を incoming 退避する（要件7.3・最上位原則）。
    ///
    /// CLAUDE.md 判断ガイド「上書きする→退避が先（確認ダイアログより退避が先）」を保存経路でも守る。
    /// 外部変更（未確認）状態のまま保存すると、その外部変更が無退避で失われうる（eval high data 対応）。
    /// ここで現ディスク内容を退避してから呼び出し側が `atomic_write` で上書きする。
    ///
    /// 退避要否の判定（無駄退避を避ける）:
    /// - ディスク内容がベースラインと一致（外部変更なし）なら退避不要＝`Ok(false)`。
    /// - ディスク内容が保存しようとしている内容と一致（自分が書いた内容で実質変化なし）なら退避不要。
    /// - それ以外（ディスクに未確認の外部変更がある）は incoming 退避してから `Ok(true)`。
    ///   内容を保存できない方針（機密/10MB以上/画像）はハッシュのみで退避対象 object を持てないため
    ///   退避をスキップする（要件9.1/9.2・差分/巻き戻し非対象と整合）。
    ///
    /// 退避（object 登録＋索引追加）に失敗したら `Err` を返し、呼び出し側は**保存を中断**する
    /// （退避が取れないまま破壊的上書きをしない＝データを失わない）。
    pub fn stash_incoming_before_overwrite(
        &self,
        path: &str,
        disk_content: &str,
        incoming_content: &str,
    ) -> Result<bool, String> {
        let mut inner = self.inner.lock().map_err(|_| "snapshot ロック失敗")?;
        let key = normalize_path_key(path);
        let disk_hash = hash_normalized(disk_content);

        // 保存しようとしている内容と同じなら実質変化なし＝退避不要。
        if disk_hash == hash_normalized(incoming_content) {
            return Ok(false);
        }
        // ベースラインと一致（外部変更なし）なら退避不要。
        if let Some(baseline) = inner.store.baseline(&key) {
            if baseline.content_hash == disk_hash {
                return Ok(false);
            }
        }
        // 内容を保存できない方針（機密/10MB以上/画像）は退避対象 object を持てない（要件9.1/9.2）。
        if !baseline_policy(path, disk_content.len() as u64).stores_content() {
            return Ok(false);
        }

        // 未確認の外部変更がディスクにある。incoming として退避してから上書きを許す。
        let normalized = pika_core::diff::normalize_lf(disk_content);
        inner.objects.insert(disk_hash.clone(), normalized.clone());
        let stashed =
            inner
                .store
                .add_stash(&key, StashKind::Incoming, disk_hash.clone(), now_ms(), true);
        // 索引へ退避が入ったことを確認（入らなければ退避を握り潰さず中断＝データを失わない）。
        if stashed.stashed_object != disk_hash {
            return Err("incoming 退避を索引へ登録できませんでした（保存を中断）".into());
        }
        // 退避 object とその自己記述メタを永続化してから index を書く（最後の砦を再起動を跨いで残す・#3）。
        if let Some(dir) = inner.snap_dir.clone() {
            crate::snapshot_persist::persist_object(&dir, &disk_hash, &normalized);
            if let Some(meta) = inner.store.object_meta(&disk_hash) {
                crate::snapshot_persist::persist_meta(&dir, &disk_hash, &meta.clone());
            }
        }
        persist_index_locked(&mut inner);
        Ok(true)
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

/// 現在の索引を index.json へ永続化する（snap_dir が None ならスキップ＝後方互換・#3）。
///
/// 各 mutation の完了直前に1回呼ぶ。失敗（FS 権限等）は致命にせず警告ログのみ
/// （退避自体は索引に入っており、object/メタは別途書かれている＝最後の砦は残る）。
fn persist_index_locked(inner: &mut SnapshotInner) {
    let Some(dir) = inner.snap_dir.clone() else {
        return; // 永続化先未設定＝従来どおりメモリ保持のみ。
    };
    if let Err(e) = crate::snapshot_persist::persist_index(&dir, &inner.store) {
        crate::diagnostic::record(
            pika_core::diagnostic::LogLevel::Warn,
            "snapshot",
            "persist_index",
            None,
            &e,
        );
    }
}

/// content object を引く（遅延ロード対応＝#3）。
///
/// インメモリキャッシュ（`objects`）にあればそれを返す。ミスしたとき `snap_dir` が `Some` なら
/// `objects/<hash>.zst` から遅延ロードしてキャッシュへ挿入する（起動時に全件読まない＝固まらない）。
/// `snap_dir` が `None`（永続化なし）なら従来どおりキャッシュのみ参照する。
fn get_object(inner: &mut SnapshotInner, hash: &str) -> Option<String> {
    if let Some(content) = inner.objects.get(hash) {
        return Some(content.clone());
    }
    let dir = inner.snap_dir.clone()?;
    let loaded = crate::snapshot_persist::load_object(&dir, hash)?;
    inner.objects.insert(hash.to_string(), loaded.clone());
    Some(loaded)
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
    _access: State<'_, crate::access::AccessControl>,
) -> Result<FileDiffDto, String> {
    // #5: compute_file_diff は FS 読みをしない（current は frontend が渡す）。新規パスは canonicalize で
    // 落ちうるため、ここでは健全性検査（絶対パス/制御文字/ADS/相対拒否）のみを通す。FS 読みのある
    // confirm_file/rollback_file 側で verify_read による封じ込めを担保する（一貫性のための入口検査）。
    pika_core::path_verify::verify_received_path(&path).map_err(|e| e.to_string())?;
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

    // ベースライン内容は遅延ロード対応で引く（再起動後はキャッシュが空なので snap_dir から読む・#3）。
    let baseline_content = get_object(&mut inner, &object_hash).unwrap_or_default();
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
pub fn confirm_file(
    path: String,
    snapshot: State<'_, SnapshotService>,
    access: State<'_, crate::access::AccessControl>,
) -> Result<bool, String> {
    // #5: ディスク再読みするため verify_read で封じ込める。索引キーは baseline 設定時と同じ
    // 元パス（正規化前）由来にする（key の一致を保つ）。FS 読みは canon 実体パスで行う。
    let canon = access.verify_read(&path)?;
    let mut inner = snapshot.inner.lock().map_err(|_| "snapshot ロック失敗")?;
    let key = normalize_path_key(&path);

    let Some(frozen) = inner.diff_snapshots.get(&key).cloned() else {
        return Err("差分を表示してから確認済みにしてください".into());
    };

    // 確定直前にディスクを実読みして再照合（mtime 据え置きの取りこぼし対策にハッシュも見る）。
    let disk_content =
        std::fs::read_to_string(&canon).map_err(|e| format!("ディスク再読みに失敗: {e}"))?;
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
                inner.objects.insert(content_hash.clone(), normalized.clone());
                inner
                    .store
                    .set_baseline_with_content(&key, content_hash.clone(), content_hash.clone());
                // 新ベースライン内容 object を永続化（次回起動で差分の素を失わない・#3）。
                if let Some(dir) = inner.snap_dir.clone() {
                    crate::snapshot_persist::persist_object(&dir, &content_hash, &normalized);
                }
            } else {
                inner.store.set_baseline_hash_only(&key, content_hash);
            }
            inner.diff_snapshots.remove(&key);
            persist_index_locked(&mut inner);
            Ok(true)
        }
    }
}

/// 「確認済み時点に戻す」（ファイル単位巻き戻し・要件8.3・7.3 退避不能ガード）。
///
/// 退避不能（ベースラインがハッシュのみ／現在内容が10MB以上・画像）はブロックする。
/// 許可される場合は現在内容を rollback 退避してからベースライン内容を返す（呼び出し側が上書き）。
#[tauri::command]
pub fn rollback_file(
    path: String,
    snapshot: State<'_, SnapshotService>,
    access: State<'_, crate::access::AccessControl>,
) -> Result<String, String> {
    // #5: ディスク再読みするため verify_read で封じ込める。索引キーは元パス由来（baseline と一致）。
    let canon = access.verify_read(&path)?;
    let mut inner = snapshot.inner.lock().map_err(|_| "snapshot ロック失敗")?;
    let key = normalize_path_key(&path);

    let Some(baseline) = inner.store.baseline(&key).cloned() else {
        return Err("ベースラインがありません".into());
    };
    let Some(object_hash) = baseline.object_hash.clone() else {
        return Err("内容ベースラインが無いため巻き戻せません（10MB以上/画像）".into());
    };

    let disk_content =
        std::fs::read_to_string(&canon).map_err(|e| format!("ディスク再読みに失敗: {e}"))?;
    let current_storable = baseline_policy(&path, disk_content.len() as u64).stores_content();

    // pika-core の退避不能ガードで判定（要件7.3）。
    decide_rollback(baseline.has_content(), current_storable).map_err(|e| e.to_string())?;

    // 現在内容を rollback 退避（退避が最後の砦＝確認ダイアログより退避が先）。
    let cur_hash = hash_normalized(&disk_content);
    let normalized = pika_core::diff::normalize_lf(&disk_content);
    inner.objects.insert(cur_hash.clone(), normalized.clone());
    let stash = inner
        .store
        .add_stash(&key, StashKind::Rollback, cur_hash.clone(), now_ms(), true);
    // 退避が索引へ入ったことを確認（incoming 同様 release でも実検証＝データを失わない・#1）。
    // 入らなければ退避を握り潰さず中断する（無退避のまま破壊的上書きをしない）。
    if stash.stashed_object != cur_hash {
        return Err("巻き戻し退避を索引へ登録できませんでした（巻き戻し中断）".into());
    }
    // 退避 object とメタを永続化（再起動を跨いで巻き戻し退避を残す・#3）。
    if let Some(dir) = inner.snap_dir.clone() {
        crate::snapshot_persist::persist_object(&dir, &cur_hash, &normalized);
        if let Some(meta) = inner.store.object_meta(&cur_hash) {
            crate::snapshot_persist::persist_meta(&dir, &cur_hash, &meta.clone());
        }
    }

    // ベースライン内容を返す（呼び出し側がディスク/バッファへ上書きする）。
    // ベースライン object が欠損していたら空内容で上書きさせない＝中断する（#1・最上位原則1）。
    // 遅延ロード対応で引く（再起動後はキャッシュが空なので snap_dir から読む・#3）。
    let Some(baseline_content) = get_object(&mut inner, &object_hash) else {
        return Err("ベースライン内容が見つかりません（巻き戻し中断）".into());
    };
    persist_index_locked(&mut inner);
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
        // ディスク再読みに失敗したファイルは **このバッチで確定しない**＝未読維持で対象外にする（#2）。
        // 読み取り失敗を空文字扱いするとベースラインが空内容へ確定しうる（confirm_file は ? で中断する）。
        let disk_content = match std::fs::read_to_string(path) {
            Ok(c) => c,
            Err(_) => continue,
        };
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
                        obj.clone(),
                        now,
                        false, // baseline-replace は LRU 枠を消費しない。
                    );
                    // 退避 object（=従来ベースライン内容）の自己記述メタを永続化（一括取り消しの素・#3）。
                    // object 実体はベースライン化時（capture/confirm）に既に永続化済み。
                    if let Some(dir) = inner.snap_dir.clone() {
                        if let Some(meta) = inner.store.object_meta(&obj) {
                            crate::snapshot_persist::persist_meta(&dir, &obj, &meta.clone());
                        }
                    }
                    result.stashed += 1;
                }
                // ベースラインを差分時点（フリーズ内容＝現ディスク）へ更新。
                if store_content {
                    if let Some(disk) = disk_map.get(&rel_path) {
                        let normalized = pika_core::diff::normalize_lf(disk);
                        inner.objects.insert(content_hash.clone(), normalized.clone());
                        // 新ベースライン内容 object を永続化（次回起動で差分の素を失わない・#3）。
                        if let Some(dir) = inner.snap_dir.clone() {
                            crate::snapshot_persist::persist_object(
                                &dir,
                                &content_hash,
                                &normalized,
                            );
                        }
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

    // バッチ全体の更新を1回でまとめて永続化（mutation 完了直前に index を1回・#3）。
    persist_index_locked(&mut inner);
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

/// これまでに `now_ms()` が返した最大値（単調性のフロア）。
///
/// 退避の created_at_ms は 14日保護（容量GC）の窓判定に使う。クロックが UNIX_EPOCH 前へ
/// 後退すると `duration_since` が Err → 従来は 0 を返し、created_at_ms=0 の退避が量産されて
/// GC の14日保護が外れていた（#16）。LRU/押し出し順は pika-core 側の単調増加 seq が主キーなので
/// 順序は壊れないが、保護窓の壁時計値が後退しないよう本フロアで単調化する。
static NOW_MS_FLOOR: AtomicU64 = AtomicU64::new(0);

/// 現在時刻（ミリ秒・単調化）。退避の created_at に使う。
///
/// 取得失敗（クロック後退）時も 0 へ落とさず、直近に返した値を再利用して単調性を壊さない（#16）。
fn now_ms() -> u64 {
    let wall = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .ok();
    let prev = NOW_MS_FLOOR.load(Ordering::Relaxed);
    // 壁時計が取れたら prev とのより大きい方、取れなければ prev を採用（後退・0 化を防ぐ）。
    let value = wall.map(|w| w.max(prev)).unwrap_or(prev);
    // フロアを前進させる（並行更新でも最大値へ収束させる）。
    NOW_MS_FLOOR.fetch_max(value, Ordering::Relaxed);
    value
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering as AtomicOrdering};

    /// テスト用 data_root（衝突回避に nanos＋連番）。
    fn temp_data_root(tag: &str) -> PathBuf {
        static SEQ: AtomicU64 = AtomicU64::new(0);
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        let seq = SEQ.fetch_add(1, AtomicOrdering::Relaxed);
        let dir = std::env::temp_dir().join(format!("pika-snap-{tag}-{nanos}-{seq}"));
        std::fs::create_dir_all(&dir).expect("data_root 作成");
        dir
    }

    /// service 内の索引（store）へテスト専用にアクセスして検証する補助。
    impl SnapshotService {
        fn with_inner<R>(&self, f: impl FnOnce(&SnapshotInner) -> R) -> R {
            let inner = self.inner.lock().expect("lock");
            f(&inner)
        }
        /// インメモリキャッシュをクリアして「遅延ロードでしか取れない」状況を作る。
        fn clear_object_cache(&self) {
            self.inner.lock().expect("lock").objects.clear();
        }
        /// 遅延ロード経路で object を取得する（テスト検証用）。
        fn fetch_object(&self, hash: &str) -> Option<String> {
            let mut inner = self.inner.lock().expect("lock");
            get_object(&mut inner, hash)
        }
        fn snap_dir(&self) -> Option<PathBuf> {
            self.inner.lock().expect("lock").snap_dir.clone()
        }
    }

    #[test]
    fn 永続化往復_再起動でベースラインと退避が揮発しない() {
        let data_root = temp_data_root("roundtrip");
        let ws = data_root.join("ws").to_string_lossy().to_string();

        // --- セッション1: ベースライン取得＋incoming 退避を行い、永続化する。 ---
        let s1 = SnapshotService::new();
        s1.set_workspace(&ws, &data_root).expect("set_workspace");
        let file = format!("{ws}/a.md");
        // 初回ベースライン（全既読スタート）。
        s1.capture_baseline(&file, "ベースライン本文\n", 30);
        let base_hash = hash_normalized("ベースライン本文\n");
        // ディスクに未確認の外部変更がある状況で上書き保存直前の incoming 退避。
        let stashed = s1
            .stash_incoming_before_overwrite(&file, "外部が書いた本文\n", "自分の保存本文\n")
            .expect("stash");
        assert!(stashed, "未確認の外部変更は incoming 退避される");
        let incoming_hash = hash_normalized("外部が書いた本文\n");

        // index.json と object .zst が実ファイルとして書かれている。
        let snap_dir = s1.snap_dir().expect("snap_dir");
        assert!(
            snap_dir.join(crate::snapshot_persist::INDEX_FILE).exists(),
            "index.json が永続化される"
        );
        let objects = snap_dir.join(crate::snapshot_persist::OBJECTS_DIR);
        assert!(
            objects.join(format!("{base_hash}.zst")).exists(),
            "ベースライン object が永続化される"
        );
        assert!(
            objects.join(format!("{incoming_hash}.zst")).exists(),
            "incoming 退避 object が永続化される"
        );
        drop(s1); // セッション1 終了（プロセス終了相当・インメモリは破棄）。

        // --- セッション2: 新しい service で同じ snap_dir をロードする。 ---
        let s2 = SnapshotService::new();
        s2.set_workspace(&ws, &data_root).expect("再オープン");
        let key = normalize_path_key(&file);
        // ベースラインがロードされている（再起動で揮発しない）。
        s2.with_inner(|inner| {
            let b = inner.store.baseline(&key).expect("ベースラインがロードされる");
            assert_eq!(b.content_hash, base_hash);
            assert_eq!(b.object_hash.as_deref(), Some(base_hash.as_str()));
            // incoming 退避もロードされている。
            assert!(
                inner
                    .store
                    .stashes(&key)
                    .iter()
                    .any(|e| e.object_hash == incoming_hash),
                "incoming 退避がロードされる"
            );
        });
        // object 内容は遅延ロードで取れる（キャッシュ空でも snap_dir から読む）。
        s2.clear_object_cache();
        assert_eq!(
            s2.fetch_object(&base_hash).as_deref(),
            Some("ベースライン本文\n"),
            "ベースライン object を遅延ロードで取得できる"
        );
        assert_eq!(
            s2.fetch_object(&incoming_hash).as_deref(),
            Some("外部が書いた本文\n"),
            "退避 object を遅延ロードで取得できる"
        );

        // --- 非クロバー: 再オープンで capture_baseline しても永続ベースラインを上書きしない。 ---
        s2.capture_baseline(&file, "全く別の内容\n", 18);
        s2.with_inner(|inner| {
            let b = inner.store.baseline(&key).expect("ベースライン");
            assert_eq!(
                b.content_hash, base_hash,
                "既存ベースラインは再オープンの capture で上書きされない（非クロバー）"
            );
        });

        let _ = std::fs::remove_dir_all(&data_root);
    }

    #[test]
    fn 破損復元_index_が壊れても退避メタから最後の砦へ到達する() {
        let data_root = temp_data_root("recover");
        let ws = data_root.join("ws").to_string_lossy().to_string();

        // セッション1: 退避を積んで永続化する。
        let s1 = SnapshotService::new();
        s1.set_workspace(&ws, &data_root).expect("set_workspace");
        let file = format!("{ws}/b.md");
        s1.capture_baseline(&file, "原本\n", 10);
        s1.stash_incoming_before_overwrite(&file, "外部変更\n", "保存内容\n")
            .expect("stash");
        let incoming_hash = hash_normalized("外部変更\n");
        let snap_dir = s1.snap_dir().expect("snap_dir");
        // 退避 object の自己記述メタが書かれている（破損復元の素）。
        let meta_path = snap_dir
            .join(crate::snapshot_persist::OBJECTS_DIR)
            .join(format!("{incoming_hash}.meta.json"));
        assert!(meta_path.exists(), "退避 object の自己記述メタが永続化される");
        drop(s1);

        // index.json を壊す（不正 JSON で上書き）。
        let index_path = snap_dir.join(crate::snapshot_persist::INDEX_FILE);
        std::fs::write(&index_path, b"{ this is not valid json ").expect("index 破壊");

        // セッション2: 破損 index でも object メタから退避が復元される（最後の砦）。
        let s2 = SnapshotService::new();
        s2.set_workspace(&ws, &data_root)
            .expect("破損 index でも set_workspace は致命にならない");
        let key = normalize_path_key(&file);
        s2.with_inner(|inner| {
            let stashes = inner.store.stashes(&key);
            assert!(
                stashes.iter().any(|e| e.object_hash == incoming_hash),
                "破損 index でも退避メタから退避が再生成される（最後の砦・要件9.1）"
            );
            assert!(
                stashes.iter().all(|e| e.unrestored),
                "復元された退避は復元待ち（unrestored=true）として提示される"
            );
            // ベースラインは復元しない（内容照合で別途取り直す前提＝design doc 11章）。
            assert!(
                inner.store.baseline(&key).is_none(),
                "破損復元ではベースラインは復元しない"
            );
        });
        // 退避 object 実体は遅延ロードで取得できる（最後の砦の内容に到達）。
        s2.clear_object_cache();
        assert_eq!(
            s2.fetch_object(&incoming_hash).as_deref(),
            Some("外部変更\n"),
            "復元された退避 object の内容に到達できる"
        );

        let _ = std::fs::remove_dir_all(&data_root);
    }

    #[test]
    fn snap_dir_未設定なら従来どおりメモリ保持で動く_後方互換() {
        // set_workspace を呼ばない＝snap_dir=None。永続化はスキップされるが API は従来どおり動く。
        let s = SnapshotService::new();
        assert!(s.snap_dir().is_none());
        s.capture_baseline("/tmp/c.md", "本文\n", 8);
        let key = normalize_path_key("/tmp/c.md");
        s.with_inner(|inner| {
            assert!(
                inner.store.baseline(&key).is_some(),
                "snap_dir 未設定でもベースラインはメモリ保持される"
            );
        });
        // 永続化されていない（snap_dir が None なので index 書込もない）。
        assert!(s.snap_dir().is_none());
    }
}
