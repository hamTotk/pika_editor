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
    decide_confirm, decide_confirm_all, decide_rollback, ConfirmAllOutcome, ConfirmAllTarget,
    ConfirmDecision, DiffSnapshot, DiskState,
};
use pika_core::snapshot::{
    baseline_policy_with, hash_normalized, BaselinePolicy, SnapshotStore, StashKind,
};
use serde::Serialize;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::UNIX_EPOCH;
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
    /// 設定 `sensitive_patterns`（既定パターンに**足す**glob・和集合＝U2b-2）。
    /// 既定（空＝`Vec::new()`）は既定パターンのみ。`open_workspace` が [`SnapshotService::set_sensitive_patterns`]
    /// で settings から流し込む。ベースライン判定（baseline_policy_with）の機密和集合に使い、機密該当を
    /// HashOnly（平文を残さない＝要件9.1）へ倒す。**既定機密は外せない**（is_sensitive_with が常に内包）。
    /// 取得不能時は空のまま＝既定のみで安全側（機密判定を弱めない不変条件）。
    /// mid-session のパターン変更による既存 baseline 貼り直しは MVP 外（open 時反映で可）。
    sensitive_patterns: Vec<String>,
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

    /// 設定 `sensitive_patterns`（機密判定の和集合＝U2b-2）をベースライン判定へ流し込む。
    ///
    /// `open_workspace` が baseline ループ**前**に呼ぶ。以後 baseline_policy_with の機密判定が
    /// 「既定 ∪ ここで渡した patterns」になり、設定該当ファイルも HashOnly（平文を残さない＝要件9.1）。
    /// **既定機密は外せない**（is_sensitive_with が常に既定を内包）＝設定は足すだけ（減らせない不変条件）。
    /// 毒化（ロック失敗）時は握り潰す（設定反映に失敗しても既定のみで安全側に倒れる＝弱めない）。
    pub fn set_sensitive_patterns(&self, patterns: Vec<String>) {
        if let Ok(mut inner) = self.inner.lock() {
            inner.sensitive_patterns = patterns;
        }
    }

    /// フォルダ初回オープン時にベースラインを取得する（全既読スタート＝要件8.1・index 永続化は遅延）。
    /// 内容保存方針（機密/10MB以上/画像＝ハッシュのみ）は pika-core::snapshot::policy で判定する。
    ///
    /// **非クロバー化（#3）**: 既にベースラインがある path はスキップする。再オープン時に
    /// 永続済みベースライン（前回確認済み内容）を「全既読」で上書きしてしまうと、閉じている間の
    /// 外部変更が差分として残らない。要件8.1「全既読スタート」は **初回（ベースライン未保持）の
    /// ファイルにのみ**適用するのが正しい挙動。
    ///
    /// **バッチ作法（性能・設計原則2「固まらない」）**: 内容 object（.zst・write-once）はここで即時
    /// 永続化する（データを失わない＝クラッシュしても次回 open の再 capture で recover_from_meta により
    /// 内容に到達できる安全網を保つ）。**遅延するのは index.json の集約書き込みのみ**で、フォルダ
    /// オープンのように N 件を連続 capture したあと呼び出し側が [`Self::persist_index`] を **1回だけ**
    /// 呼んで永続化する（N 件で N 回のフル BTreeMap 直列化＋fsync を1回へ畳む）。
    pub fn capture_baseline(&self, path: &str, content: &str, size: u64) {
        let mut inner = self.inner.lock().expect("snapshot lock");
        capture_baseline_locked(&mut inner, path, content, size);
        // index は永続化しない（ループ後に persist_index で1回まとめる）。
    }

    /// フォルダ初回オープン時に**ハッシュのみ**ベースラインを取得する（バイナリ等・#54・index 永続化は遅延）。
    ///
    /// テキストでない（encoding::decode が strict 失敗し lossy へ倒れた＝had_decode_warning）ファイルは
    /// 内容 object を持たない（差分/巻き戻し非対象）。外部変更検知用にバイト由来ハッシュをベースラインへ据える
    /// （空文字を内容ベースライン化しない＝全ファイル空ハッシュ衝突を避ける・#54。ロッシーデコードした
    /// バイナリ内容を data root へ保存しない＝データ最小化#20・肥大/folder-open コスト回避・第2巡 回帰修正）。
    /// [`Self::capture_baseline`] と同じく **非クロバー**で、内容 object を持たないため遅延するのは index.json のみ。
    pub fn capture_baseline_hash_only(&self, path: &str, content_hash: &str) {
        let mut inner = self.inner.lock().expect("snapshot lock");
        capture_baseline_hash_only_locked(&mut inner, path, content_hash);
        // index は永続化しない（ループ後に persist_index で1回まとめる）。
    }

    /// 文書を**開いた時点**の内容を非クロバーでベースライン化する（要件8.1 全既読スタート・指摘6）。
    ///
    /// `open_workspace` の直下ループはルート直下ファイルしかベースラインを張らない。サブフォルダを
    /// 遅延展開して開いたファイルはベースライン不在のままになり、`compute_file_diff` がそれを
    /// `has_baseline_content:false` で返すため、フロントが誤って「差分非対象（10MB以上/画像/機密）」を
    /// 全ファイルに表示していた。`open_document` から本メソッドを呼び、開いた瞬間の内容を直下ファイルと
    /// 同じ「全既読スタート」へ揃える（編集前に張るので、以後の編集は差分として正しく現れる）。
    ///
    /// 非クロバー（既存ベースラインは触らない）かつ policy 準拠（機密/巨大/画像は `capture_baseline_locked`
    /// が踏襲してハッシュのみ＝差分非対象のまま正しく扱う）。**新規に取得したときだけ** index を
    /// 永続化する（毎オープンの per-file fsync を避ける＝固まらない）。取得済み path は早期 return で no-op。
    pub fn ensure_baseline(&self, path: &str, content: &str, size: u64) {
        let Ok(mut inner) = self.inner.lock() else {
            return; // ロック毒化は致命にしない（ベースライン未取得でも編集体験は継続）。
        };
        let key = normalize_path_key(path);
        if inner.store.baseline(&key).is_some() {
            return; // 直下ループ取得済み/前回オープン永続分＝非クロバー（no-op）。
        }
        // 機密/巨大/画像（policy=HashOnly）は内容を保存しない方針。open_workspace 経路
        // （capture_baseline_for）が `capture_baseline(path, "", size)` と**空文字でハッシュ化**するのに対し、
        // 旧実装はここで実内容 `content` を渡していたため、同一機密ファイルを「直下で開く」のと
        // 「サブフォルダで開く」のとで baseline content_hash が食い違っていた（指摘3）。
        // HashOnly のときは空文字でハッシュ化して両経路を一致させ、機密の平文をハッシュ入力にもしない
        // （より安全側）。StoreContent のときだけ実内容で張る（capture_baseline_locked が policy を再判定）。
        let content_for_baseline =
            if baseline_policy_with(path, size, &inner.sensitive_patterns).stores_content() {
                content
            } else {
                ""
            };
        capture_baseline_locked(&mut inner, path, content_for_baseline, size);
        // 単発取得なので、ここで index を1回永続化する（新規サブフォルダファイルの初回オープン時のみ）。
        persist_index_locked(&mut inner);
    }

    /// 索引（index.json）を1回だけ永続化する（バッチ capture のループ完了後に呼ぶ）。
    ///
    /// `capture_baseline*` で遅延していた index 永続化をここでまとめて行う。`snap_dir` が `None`
    /// （永続化なし）なら no-op。失敗は致命にせず警告ログのみ（object/メタは個別に永続化済み＝最後の砦は残る）。
    pub fn persist_index(&self) {
        let mut inner = self.inner.lock().expect("snapshot lock");
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
        // 機密判定は設定 sensitive_patterns 和集合（既定は外せない＝U2b-2）。
        if !baseline_policy_with(path, disk_content.len() as u64, &inner.sensitive_patterns)
            .stores_content()
        {
            return Ok(false);
        }

        // 未確認の外部変更がディスクにある。incoming として退避してから上書きを許す。
        // 退避（object 登録＋索引追加＋実検証＋object/メタ永続化）は共通フローへ集約した（rollback と共用）。
        stash_and_persist_locked(
            &mut inner,
            &key,
            StashKind::Incoming,
            disk_content,
            "incoming 退避を索引へ登録できませんでした（保存を中断）",
        )?;
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

/// ベースライン取得の本体（ロック取得済み・index 永続化は呼び出し側に委ねる）。
///
/// [`SnapshotService::capture_baseline`] の実体。内容 object（.zst・write-once）はここで即時
/// 永続化する（データを失わない＝recover_from_meta の素を残す）。index.json は **ここでは書かない**
/// （呼び出し側がループ後に [`SnapshotService::persist_index`] で1回まとめて永続化する）。非クロバー維持。
fn capture_baseline_locked(inner: &mut SnapshotInner, path: &str, content: &str, size: u64) {
    let key = normalize_path_key(path);
    // 既にベースラインがある（前回オープンで永続化された）path は触らない（非クロバー）。
    if inner.store.baseline(&key).is_some() {
        return;
    }
    // 機密判定は設定 sensitive_patterns 和集合（既定は外せない＝U2b-2）。
    match baseline_policy_with(path, size, &inner.sensitive_patterns) {
        // 内容ベースライン設定＋object 永続化は共通フローへ集約（confirm/confirm_all と共用）。
        BaselinePolicy::StoreContent => set_baseline_and_persist(inner, &key, content),
        BaselinePolicy::HashOnly => {
            inner
                .store
                .set_baseline_hash_only(&key, hash_normalized(content));
        }
    }
}

/// ハッシュのみベースライン取得の本体（ロック取得済み・index 永続化は呼び出し側に委ねる）。
///
/// [`SnapshotService::capture_baseline_hash_only`] の実体。内容 object は持たない（HashOnly）。
/// index.json はここでは書かない（呼び出し側がループ後に persist_index で1回）。非クロバー維持。
fn capture_baseline_hash_only_locked(inner: &mut SnapshotInner, path: &str, content_hash: &str) {
    let key = normalize_path_key(path);
    // 既にベースラインがある（前回オープンで永続化された）path は触らない（非クロバー）。
    if inner.store.baseline(&key).is_some() {
        return;
    }
    // 内容 object は持たない（HashOnly）。ハッシュのみを索引へ据える。
    inner
        .store
        .set_baseline_hash_only(&key, content_hash.to_string());
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

/// object の自己記述メタを永続化する（snap_dir が Some のときだけ・最後の砦の素を残す）。
///
/// メタが残っていれば index.json が壊れても `recover_stashes_from_meta` で退避を再生成できる。
/// 退避（incoming/rollback）・baseline-replace 退避が共用する（重複していた meta 永続化ブロックを集約）。
fn persist_meta_locked(inner: &SnapshotInner, hash: &str) {
    let Some(dir) = inner.snap_dir.clone() else {
        return;
    };
    if let Some(meta) = inner.store.object_meta(hash) {
        crate::snapshot_persist::persist_meta(&dir, hash, &meta.clone());
    }
}

/// object 実体とその自己記述メタを永続化する（snap_dir が Some のときだけ）。
///
/// 退避フロー（incoming/rollback）の「object＋メタを即時永続化」ブロックを集約する（最後の砦を
/// 再起動を跨いで残す＝#3）。object は write-once で小さく即時永続化がデータ安全網（recover_from_meta）。
fn persist_object_and_meta(inner: &SnapshotInner, hash: &str, normalized: &str) {
    if let Some(dir) = inner.snap_dir.clone() {
        crate::snapshot_persist::persist_object(&dir, hash, normalized);
    }
    persist_meta_locked(inner, hash);
}

/// 現内容を退避（object 登録＋索引追加＋実検証＋object/メタ永続化）する共通フロー（ロック取得済み）。
///
/// `stash_incoming_before_overwrite`（incoming）と `rollback_file`（rollback）が共用する。
/// content を LF 正規化して object 登録 → `add_stash`（LRU 枠を消費）→ **索引へ実登録されたか検証**
/// （握り潰さない＝データを失わない）→ object とメタを永続化する。索引登録に失敗したら `err_msg` を
/// 返し、呼び出し側は破壊的操作（上書き/巻き戻し）を中断する。退避した object ハッシュを返す。
fn stash_and_persist_locked(
    inner: &mut SnapshotInner,
    key: &str,
    kind: StashKind,
    content: &str,
    err_msg: &str,
) -> Result<String, String> {
    let hash = hash_normalized(content);
    let normalized = pika_core::diff::normalize_lf(content);
    inner.objects.insert(hash.clone(), normalized.clone());
    let stashed = inner
        .store
        .add_stash(key, kind, hash.clone(), crate::util::now_ms(), true);
    // 索引へ退避が入ったことを確認（入らなければ退避を握り潰さず中断＝データを失わない・#1）。
    if stashed.stashed_object != hash {
        return Err(err_msg.to_string());
    }
    persist_object_and_meta(inner, &hash, &normalized);
    Ok(hash)
}

/// 内容ベースラインを据えて object を永続化する共通フロー（ロック取得済み）。
///
/// content を LF 正規化して object 登録 → `set_baseline_with_content`（content_hash=object_hash）→
/// object 実体を永続化する（再起動で差分の素を失わない・#3）。「内容ベースラインを新しい内容へ進める」
/// 3経路（capture 初回・confirm 確認済み更新・confirm_all 一括確認）が共用する。
fn set_baseline_and_persist(inner: &mut SnapshotInner, key: &str, content: &str) {
    let hash = hash_normalized(content);
    let normalized = pika_core::diff::normalize_lf(content);
    inner.objects.insert(hash.clone(), normalized.clone());
    inner
        .store
        .set_baseline_with_content(key, hash.clone(), hash.clone());
    // ※ index と違い object は write-once で小さく、即時永続化がデータ安全網（recover_from_meta）。
    if let Some(dir) = inner.snap_dir.clone() {
        crate::snapshot_persist::persist_object(&dir, &hash, &normalized);
    }
}

/// compute_file_diff のフォールバック: ベースライン未取得（open 経路漏れ＝サブフォルダ等）なら
/// 全既読スタート（要件8.1）として現在内容をベースライン化する（指摘6）。
///
/// policy 判定（機密/巨大/画像）は open 経路と同じ**ディスク実サイズ**で行う（指摘7）。`current.len()` は
/// デコード後 UTF-8 のバイト長で、Shift_JIS 等ではディスク比で膨らみ 10MB 境界を誤判定しうるため、
/// FS 読みを避ける設計でも稀なフォールバックに限り size 取得の 1 回 stat のみ許容する。新規に取得した
/// ときだけ index を永続化する（取得済みは早期 return で no-op）。
fn ensure_diff_baseline_locked(inner: &mut SnapshotInner, path: &str, current: &str) {
    if inner.store.baseline(&normalize_path_key(path)).is_some() {
        return;
    }
    let disk_size = std::fs::metadata(path)
        .map(|m| m.len())
        .unwrap_or(current.len() as u64);
    capture_baseline_locked(inner, path, current, disk_size);
    persist_index_locked(inner);
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

    // ベースライン未取得（open_workspace の直下ループ外＝サブフォルダ等で開かれたファイル）。
    // 通常は open_document の ensure_baseline で取得済みだが、経路漏れに備え compute 時にも全既読
    // スタート（要件8.1）として現在内容をベースライン化する。これをしないと「ベースライン不在」を
    // has_baseline_content:false で返し、フロントが誤って「差分非対象（10MB以上/画像/機密）」を
    // 通常ファイルにも出していた（指摘6）。policy は capture が踏襲する（機密/巨大はハッシュのみのまま）。
    ensure_diff_baseline_locked(&mut inner, &path, &current);

    let Some(baseline) = inner.store.baseline(&key).cloned() else {
        // capture が失敗してもパニックさせない。全既読スタート相当（差分なし）として返す
        // （誤った「差分非対象」は出さない＝指摘6）。
        return Ok(FileDiffDto {
            lines: Vec::new(),
            change_count: 0,
            has_baseline_content: true,
        });
    };

    let Some(object_hash) = baseline.object_hash.clone() else {
        // ハッシュのみ＝**真に**差分非対象（機密/10MB以上/画像＝要件8.2/9.2）。これは正しい挙動
        //（通常の小さなテキストはこの分岐に来ない＝上で内容ベースラインを張るため）。
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
    // FS 操作（読み・mtime・policy）は canon 実体パスで揃える（生パスと canon が別表現の
    // 場合のちぐはぐを避ける）。索引キー `key` は上で生パス由来に保つ（frozen との照合不変条件）。
    let canon_str = canon.to_string_lossy();
    let disk_content =
        std::fs::read_to_string(&canon).map_err(|e| format!("ディスク再読みに失敗: {e}"))?;
    let disk = DiskState {
        mtime_ms: file_mtime_ms(&canon_str),
        content_hash: hash_normalized(&disk_content),
    };
    // 機密判定は設定 sensitive_patterns 和集合（既定は外せない＝U2b-2）。
    let policy = baseline_policy_with(
        &canon_str,
        disk_content.len() as u64,
        &inner.sensitive_patterns,
    );

    match decide_confirm(&frozen, &disk, policy) {
        ConfirmDecision::AbortReDiff => Ok(false), // 中断＝未読維持・再差分を促す。
        ConfirmDecision::UpdateBaseline {
            content_hash,
            store_content,
        } => {
            // store_content の content_hash は decide_confirm が frozen.content_hash（=確定直前に再照合した
            // disk のハッシュ）を返すため hash_normalized(disk_content) と一致する。よって共通フロー
            // （disk_content から再計算）で据えても object_hash/content_hash は同値（等価リファクタ）。
            if store_content {
                set_baseline_and_persist(&mut inner, &key, &disk_content);
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

    // FS 操作（読み・policy）は canon 実体パスで揃える。索引キー `key` は生パス由来のまま
    // 維持する（baseline との一致＝不変条件）。rollback は mtime を見ないので policy のみ canon 化。
    let disk_content =
        std::fs::read_to_string(&canon).map_err(|e| format!("ディスク再読みに失敗: {e}"))?;
    // 機密判定は設定 sensitive_patterns 和集合（既定は外せない＝U2b-2）。
    let current_storable = baseline_policy_with(
        &canon.to_string_lossy(),
        disk_content.len() as u64,
        &inner.sensitive_patterns,
    )
    .stores_content();

    // pika-core の退避不能ガードで判定（要件7.3）。
    decide_rollback(baseline.has_content(), current_storable).map_err(|e| e.to_string())?;

    // 現在内容を rollback 退避（退避が最後の砦＝確認ダイアログより退避が先）。
    // 退避（object 登録＋索引追加＋実検証＋object/メタ永続化）は incoming と同じ共通フローへ集約した。
    stash_and_persist_locked(
        &mut inner,
        &key,
        StashKind::Rollback,
        &disk_content,
        "巻き戻し退避を索引へ登録できませんでした（巻き戻し中断）",
    )?;

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
/// confirm_all の対象組み立て（フリーズした未読集合→検証済みターゲット＋ディスク内容）。
///
/// 各 path について diff_snapshots のフリーズ済み基準を引き、`verify_read` で封じ込めた canon 実体パスを
/// 読む。差分未提示／封じ込め外／読み取り失敗は **このバッチで確定しない**（per-path で外す＝未読維持・
/// #2/#5）。索引キーは生パス由来（正規化済み）を維持し、canon は FS 読み専用に使う（confirm_file と同作法。
/// キーを canon にすると frozen diff_snapshot との照合不変条件が崩れる）。
fn collect_confirm_all_targets(
    inner: &SnapshotInner,
    access: &crate::access::AccessControl,
    paths: &[String],
) -> (Vec<ConfirmAllTarget>, HashMap<String, String>) {
    let mut targets: Vec<ConfirmAllTarget> = Vec::new();
    // (key, disk_content) を退避/object 保存用に保持する。
    let mut disk_map: HashMap<String, String> = HashMap::new();
    for path in paths {
        let key = normalize_path_key(path);
        let Some(frozen) = inner.diff_snapshots.get(&key).cloned() else {
            continue; // 差分未提示のものは対象外（見ていない内容を確定しない）。
        };
        let Ok(canon) = access.verify_read(path) else {
            continue; // 封じ込め外は対象外（未読維持）。
        };
        let Ok(disk_content) = std::fs::read_to_string(&canon) else {
            continue; // ディスク再読み失敗は対象外（空文字確定を避ける・#2）。
        };
        // FS 操作（mtime・policy）は canon 実体パスで揃える。索引キー `key` は生パス由来のまま維持する。
        let canon_str = canon.to_string_lossy();
        let disk = DiskState {
            mtime_ms: file_mtime_ms(&canon_str),
            content_hash: hash_normalized(&disk_content),
        };
        // 機密判定は設定 sensitive_patterns 和集合（既定は外せない＝U2b-2）。
        let policy = baseline_policy_with(
            &canon_str,
            disk_content.len() as u64,
            &inner.sensitive_patterns,
        );
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
    (targets, disk_map)
}

/// confirm_all の outcome 適用（ベースライン更新・baseline-replace 退避・件数集計）。
///
/// `Updated` は更新前ベースライン object を baseline-replace バッチへ退避（LRU 枠とは別・要件8.3）し、
/// 内容ベースラインを差分時点（フリーズ内容＝現ディスク）へ進める。store_content の content_hash は
/// decide_confirm_all が frozen.content_hash（=disk 一致時のハッシュ）を返すため hash_normalized(disk) と
/// 一致し、disk_map から共通フローで据えても同値（等価リファクタ）。`SkippedChanged` は未読のまま残す。
fn apply_confirm_all_outcomes(
    inner: &mut SnapshotInner,
    outcomes: Vec<ConfirmAllOutcome>,
    disk_map: &HashMap<String, String>,
    now: u64,
) -> ConfirmAllResult {
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
                // object 実体はベースライン化時（capture/confirm）に永続化済みなのでメタのみ残す（一括取消の素・#3）。
                if let Some(obj) = stash_object {
                    inner.store.add_stash(
                        &rel_path,
                        StashKind::BaselineReplace,
                        obj.clone(),
                        now,
                        false, // baseline-replace は LRU 枠を消費しない。
                    );
                    persist_meta_locked(inner, &obj);
                    result.stashed += 1;
                }
                // ベースラインを差分時点（フリーズ内容＝現ディスク）へ更新。
                if store_content {
                    match disk_map.get(&rel_path) {
                        // disk_map には対象 path が必ず入っている（collect で同時挿入）。共通フローで据える。
                        Some(disk) => set_baseline_and_persist(inner, &rel_path, disk),
                        // 念のためのフォールバック（disk 欠落時は object を持たず content_hash のみ据える）。
                        None => inner.store.set_baseline_with_content(
                            &rel_path,
                            content_hash.clone(),
                            content_hash,
                        ),
                    }
                } else {
                    inner.store.set_baseline_hash_only(&rel_path, content_hash);
                }
                inner.diff_snapshots.remove(&rel_path);
                result.updated += 1;
            }
        }
    }
    result
}

/// 「すべて確認済みにする」（要件8.3）。
///
/// `paths` は実行開始時点でフロントがフリーズした未読集合。各ファイルについて
/// 確定直前にディスクを再読みして再照合し、変化していないものだけベースラインを更新する。
/// 更新前ベースライン object は baseline-replace バッチへ一括退避する（ワンクリック一括取り消し可能）。
/// 対象組み立てと outcome 適用は private 関数（[`collect_confirm_all_targets`]/
/// [`apply_confirm_all_outcomes`]）へ分割し、本体はロック取得→判定→永続化の配線に徹する。
#[tauri::command]
pub fn confirm_all(
    paths: Vec<String>,
    snapshot: State<'_, SnapshotService>,
    access: State<'_, crate::access::AccessControl>,
) -> Result<ConfirmAllResult, String> {
    let mut inner = snapshot.inner.lock().map_err(|_| "snapshot ロック失敗")?;
    // 退避 created_at に使う時刻（単調化）。バッチ内の全退避で同一値を使う。
    let now = crate::util::now_ms();
    // 実行開始時点の未読集合をフリーズして検証済みターゲットを組み立てる（要件8.3）。
    let (targets, disk_map) = collect_confirm_all_targets(&inner, &access, &paths);
    let outcomes = decide_confirm_all(&targets);
    let result = apply_confirm_all_outcomes(&mut inner, outcomes, &disk_map, now);
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

// 退避 created_at・時間窓判定の単調化ミリ秒時計（旧 now_ms/NOW_MS_FLOOR）は
// src-tauri 共通の [`crate::util::now_ms`] へ集約した（watcher.rs と単一実装を共有）。

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering as AtomicOrdering};
    use std::time::SystemTime;

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
            let b = inner
                .store
                .baseline(&key)
                .expect("ベースラインがロードされる");
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
        assert!(
            meta_path.exists(),
            "退避 object の自己記述メタが永続化される"
        );
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
    fn バッチcapture_ループ後persist_index_1回でベースラインが永続化される() {
        // open_workspace 経路を模す: 複数ファイルを capture_baseline（index 遅延）でループ取得し、
        // 最後に persist_index() を1回だけ呼ぶ。再起動相当の別 service ロードでベースラインが残ることを検証。
        let data_root = temp_data_root("batch");
        let ws = data_root.join("ws").to_string_lossy().to_string();

        let s1 = SnapshotService::new();
        s1.set_workspace(&ws, &data_root).expect("set_workspace");
        let snap_dir = s1.snap_dir().expect("snap_dir");
        let index_path = snap_dir.join(crate::snapshot_persist::INDEX_FILE);

        // 3件ループ取得（この間 index.json は書かれない＝per-file fsync が無い）。
        let files: Vec<(String, String)> = (0..3)
            .map(|i| {
                let p = format!("{ws}/f{i}.md");
                let body = format!("本文{i}\n");
                s1.capture_baseline(&p, &body, body.len() as u64);
                (p, body)
            })
            .collect();

        // ループ中は index.json が書かれていないこと（per-file 永続化が無い＝固まらない）を確認。
        assert!(
            !index_path.exists(),
            "capture_baseline 単体では index.json を書かない（ループ後 persist にまとめる）"
        );
        // ループ後に1回だけ persist_index する。
        s1.persist_index();
        assert!(
            index_path.exists(),
            "ループ後の persist_index で index.json が書かれる"
        );
        drop(s1);

        // 別 service で再ロード＝再起動相当。全ベースラインがロードされる（バッチでも揮発しない）。
        let s2 = SnapshotService::new();
        s2.set_workspace(&ws, &data_root).expect("再オープン");
        s2.with_inner(|inner| {
            for (p, body) in &files {
                let key = normalize_path_key(p);
                let b = inner
                    .store
                    .baseline(&key)
                    .expect("バッチ capture したベースラインがロードされる");
                assert_eq!(b.content_hash, hash_normalized(body));
            }
        });
        // 内容 object も遅延ロードで取れる（バッチでも object は即時永続化されている）。
        s2.clear_object_cache();
        for (_p, body) in &files {
            assert_eq!(
                s2.fetch_object(&hash_normalized(body)).as_deref(),
                Some(body.as_str()),
                "バッチ capture でも内容 object は即時永続化され遅延ロードで取れる"
            );
        }

        let _ = std::fs::remove_dir_all(&data_root);
    }

    #[test]
    fn 機密ファイルは両経路でbaseline_content_hashが一致する() {
        // 指摘3: 機密(HashOnly) は ensure_baseline（サブフォルダで開く経路）が実内容を渡し、
        // open_workspace 経路（capture_baseline_for→capture_baseline 空文字）が空文字を渡すため、
        // 同一機密ファイルで baseline content_hash が食い違っていた。ensure_baseline も HashOnly なら
        // 空文字でハッシュ化して揃える（かつ機密の平文をハッシュ入力にしない）。
        let env_path = "/ws/.env"; // 既定機密（is_sensitive）＝policy=HashOnly。
        let key = normalize_path_key(env_path);

        // open_workspace 経路（capture_baseline_for 相当）: 機密は空文字でハッシュのみ。
        let s_open = SnapshotService::new();
        s_open.capture_baseline(env_path, "", 42);
        let open_hash = s_open.with_inner(|inner| {
            inner
                .store
                .baseline(&key)
                .expect("baseline（open 経路）")
                .content_hash
                .clone()
        });

        // ensure_baseline 経路: 実内容（平文の機密値）を渡しても HashOnly なので空文字でハッシュ化される。
        let s_ensure = SnapshotService::new();
        s_ensure.ensure_baseline(env_path, "SECRET=平文の機密値\n", 42);
        let ensure_hash = s_ensure.with_inner(|inner| {
            let b = inner.store.baseline(&key).expect("baseline（ensure 経路）");
            // HashOnly なので内容 object を持たない（差分非対象）。
            assert!(
                b.object_hash.is_none(),
                "機密ファイルは内容 object を持たない（HashOnly）"
            );
            b.content_hash.clone()
        });

        assert_eq!(
            open_hash, ensure_hash,
            "機密ファイルの baseline content_hash が両経路で一致しない（指摘3）"
        );
        // 空文字ハッシュであること（機密の平文をハッシュ入力にしない＝より安全側）。
        assert_eq!(
            ensure_hash,
            hash_normalized(""),
            "機密 baseline は空文字ハッシュであるべき（平文を入力にしない）"
        );
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
