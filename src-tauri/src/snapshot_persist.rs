//! スナップショット永続化の FS I/O＋データルート DACL（#3・要件9.1/9.2・design doc 11章）。
//!
//! 役割（design doc 3章「薄い境界」）:
//! - 索引（index）・参照計数・LRU・index 破損復元の決定論ロジックは **pika-core::snapshot**（cargo test 済み）。
//! - ここには「content-addressed object（zstd）の書込/遅延読込・index.json のアトミック書込・
//!   退避メタ（自己記述）の書込・データルート最上位への DACL 適用」という FS/OS 配線のみを置く。
//!
//! 最上位原則「データを失わない／退避が最後の砦」を**再起動を跨いで**成立させるのが本モジュールの目的。
//! 現状（インメモリ保持のみ）は再起動で揮発していた（#3）。データルート配下
//! `snapshots/<wsハッシュ>/` へ object＋メタ＋index を永続化し、起動時にロード、index 破損時は
//! object メタから退避一覧を再生成する（`pika_core::snapshot::SnapshotStore::recover_stashes_from_meta`）。

use pika_core::snapshot::{zstd_compress, zstd_decompress, ObjectMeta, SnapshotStore};
use std::path::{Path, PathBuf};

/// 永続 object/メタを置くサブディレクトリ名。
pub const OBJECTS_DIR: &str = "objects";
/// 永続化索引のファイル名。
pub const INDEX_FILE: &str = "index.json";
/// 圧縮 object の拡張子。
const OBJ_EXT: &str = "zst";
/// 自己記述メタの拡張子（`<hash>.meta.json`）。
const META_EXT: &str = "meta.json";

/// `<snap_dir>/objects` への絶対パスを返す。
fn objects_dir(snap_dir: &Path) -> PathBuf {
    snap_dir.join(OBJECTS_DIR)
}

/// 圧縮 object（`objects/<hash>.zst`）の絶対パス。
fn object_path(snap_dir: &Path, hash: &str) -> PathBuf {
    objects_dir(snap_dir).join(format!("{hash}.{OBJ_EXT}"))
}

/// 自己記述メタ（`objects/<hash>.meta.json`）の絶対パス。
fn meta_path(snap_dir: &Path, hash: &str) -> PathBuf {
    objects_dir(snap_dir).join(format!("{hash}.{META_EXT}"))
}

/// content-addressed object を圧縮して書く（write-once）。
///
/// 同一ハッシュは同一内容（content-addressed）なので既に存在すれば書き直さない（重複排除）。
/// 一時ファイル→rename のアトミック書込で、途中クラッシュ時に部分内容の object を残さない。
pub fn persist_object(snap_dir: &Path, hash: &str, content: &str) {
    let target = object_path(snap_dir, hash);
    if target.exists() {
        return; // write-once（content-addressed＝同一ハッシュは同一内容）。
    }
    // zstd 圧縮は panic させず Result で受ける。失敗時（メモリ zstd では実際には起きない）は object を
    // 書かずに諦める＝既存の `atomic_write` 失敗時と同じ best-effort 挙動（観測上は不変・データ安全側）。
    if let Ok(compressed) = zstd_compress(content.as_bytes()) {
        let _ = crate::fs_atomic::atomic_write(&target, &compressed);
    }
}

/// 退避 object の自己記述メタを書く（index 破損復元の素＝最後の砦）。
///
/// メタが残っていれば index.json が壊れても `recover_stashes_from_meta` で退避を再生成できる。
/// 同一 object のメタは不変なので存在すれば書き直さない。
pub fn persist_meta(snap_dir: &Path, hash: &str, meta: &ObjectMeta) {
    let target = meta_path(snap_dir, hash);
    if target.exists() {
        return;
    }
    if let Ok(json) = serde_json::to_string(meta) {
        let _ = crate::fs_atomic::atomic_write(&target, json.as_bytes());
    }
}

/// 圧縮 object を遅延読込する（map ミス時にのみ呼ぶ＝起動を全件読込で重くしない）。
///
/// 破損（zstd 失敗・非 UTF-8）は `None`（呼び出し側は従来どおり空内容へフォールバック）。
pub fn load_object(snap_dir: &Path, hash: &str) -> Option<String> {
    let bytes = std::fs::read(object_path(snap_dir, hash)).ok()?;
    let decompressed = zstd_decompress(&bytes)?;
    String::from_utf8(decompressed).ok()
}

/// index.json を `export()` の JSON でアトミックに書く（state_store の作法に倣う）。
///
/// さらに store.object_meta の全エントリについて自己記述メタを（無ければ）書き、
/// index.json が壊れても object メタから退避を再生成できる状態を確実に残す（最後の砦）。
pub fn persist_index(snap_dir: &Path, store: &SnapshotStore) -> Result<(), String> {
    let persisted = store.export();
    let json = serde_json::to_string(&persisted).map_err(|e| format!("index 直列化に失敗: {e}"))?;
    // 退避メタの自己記述を確実に残す（破損復元の素）。
    for (hash, meta) in &persisted.object_meta {
        persist_meta(snap_dir, hash, meta);
    }
    let target = snap_dir.join(INDEX_FILE);
    crate::fs_atomic::atomic_write(&target, json.as_bytes())
        .map_err(|e| format!("index.json の保存に失敗: {e}"))
}

/// 起動ロード結果（正常ロード／破損復元／初回空）。診断ログ判定に使う。
pub enum LoadOutcome {
    /// index.json を正常にロードした。
    Loaded(SnapshotStore),
    /// index.json が壊れていたため object メタから退避を再生成した。
    RecoveredFromMeta(SnapshotStore),
    /// index.json が無い（初回ワークスペース）。空 store。
    Empty(SnapshotStore),
}

impl LoadOutcome {
    /// 復元した store を取り出す。
    pub fn into_store(self) -> SnapshotStore {
        match self {
            LoadOutcome::Loaded(s) | LoadOutcome::RecoveredFromMeta(s) | LoadOutcome::Empty(s) => s,
        }
    }
}

/// `<snap_dir>` から索引をロードする（正常／破損復元／初回空の3分岐）。
///
/// 1. index.json を読めて PersistedStore にパースできる → `SnapshotStore::import`（正常）。
/// 2. 読めない/壊れている → **破損復元**: `objects/*.meta.json` から object_meta を集め、
///    `objects/*.zst`(stem) から実在 object 集合を作り、`recover_stashes_from_meta` →
///    `install_recovered_stashes`（退避＝最後の砦に到達）。ベースラインは復元しない
///    （内容照合で別途取り直す前提＝design doc 11章）。
/// 3. index.json 不在（初回） → 空 store。
pub fn load_store(snap_dir: &Path) -> LoadOutcome {
    let index_path = snap_dir.join(INDEX_FILE);
    match std::fs::read_to_string(&index_path) {
        Ok(json) => match serde_json::from_str::<pika_core::snapshot::PersistedStore>(&json) {
            Ok(parsed) => LoadOutcome::Loaded(SnapshotStore::import(parsed)),
            Err(_) => LoadOutcome::RecoveredFromMeta(recover_from_meta(snap_dir)),
        },
        // ファイル不在は初回（空）。それ以外の読取失敗（権限等）も破損扱いで退避メタから救う。
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            LoadOutcome::Empty(SnapshotStore::new())
        }
        Err(_) => LoadOutcome::RecoveredFromMeta(recover_from_meta(snap_dir)),
    }
}

/// objects ディレクトリの自己記述メタ＋実在 object から退避一覧を再生成する（最後の砦）。
///
/// `*.meta.json` を読んで object_meta だけ詰めた store を作り、`*.zst`(stem) を実在集合として
/// `recover_stashes_from_meta` を回す。実体が欠けた object のメタは復元しない（store 側で除外）。
fn recover_from_meta(snap_dir: &Path) -> SnapshotStore {
    use pika_core::snapshot::PersistedStore;
    use std::collections::{BTreeMap, BTreeSet};

    let dir = objects_dir(snap_dir);
    let mut object_meta: BTreeMap<String, ObjectMeta> = BTreeMap::new();
    let mut present: BTreeSet<String> = BTreeSet::new();

    if let Ok(read) = std::fs::read_dir(&dir) {
        for ent in read.flatten() {
            let path = ent.path();
            let name = match path.file_name().and_then(|n| n.to_str()) {
                Some(n) => n,
                None => continue,
            };
            if let Some(hash) = name.strip_suffix(&format!(".{META_EXT}")) {
                // 自己記述メタ。
                if let Ok(text) = std::fs::read_to_string(&path) {
                    if let Ok(meta) = serde_json::from_str::<ObjectMeta>(&text) {
                        object_meta.insert(hash.to_string(), meta);
                    }
                }
            } else if let Some(hash) = name.strip_suffix(&format!(".{OBJ_EXT}")) {
                // 実体 object（走査で見つかった集合）。
                present.insert(hash.to_string());
            }
        }
    }

    // object_meta だけを持つ store を import で組み立て、退避一覧を再生成する。
    let base = SnapshotStore::import(PersistedStore {
        object_meta,
        ..PersistedStore::default()
    });
    let recovered = base.recover_stashes_from_meta(&present);
    let mut store = base;
    store.install_recovered_stashes(recovered);
    store
}

/// `<data_root>/snapshots/<wsハッシュ>` を確定し、`objects` まで作る。
///
/// ws ハッシュはパス文字列で安定化する（canonicalize できればした方がよいが、
/// 失敗時は素のパスで可＝起動を妨げない）。
pub fn snapshot_dir_for(data_root: &Path, ws_root: &str) -> Result<PathBuf, String> {
    use pika_core::hashing::hash_normalized_lf;
    // canonicalize は新規/権限不足で失敗しうる。失敗時は素のパス文字列で安定キーを作る。
    let canon = std::fs::canonicalize(ws_root)
        .map(|p| p.to_string_lossy().to_string())
        .unwrap_or_else(|_| ws_root.to_string());
    let ws_hash = hash_normalized_lf(canon.as_bytes());
    let snap_dir = data_root.join("snapshots").join(ws_hash);
    std::fs::create_dir_all(objects_dir(&snap_dir))
        .map_err(|e| format!("スナップショット領域作成に失敗: {e}"))?;
    Ok(snap_dir)
}

/// データルート最上位に所有者＋SYSTEM 限定の DACL を張る（#3・他ユーザーから退避を遮蔽）。
///
/// 退避スナップショット（最後の砦）にはユーザー文書の旧内容が含まれうる。データルートを
/// 所有者と LocalSystem のみアクセス可・継承保護にして、他ユーザーから読めない状態にする。
/// 失敗（権限・非 NTFS・data_root 解決失敗等）は **致命にしない**（Err を返すが呼び出し側は
/// 警告ログのみで起動継続＝固まらない・データを失わない方を優先）。
///
/// 非 Windows ビルドでは no-op（`Ok(())`）。
#[cfg(windows)]
pub fn harden_data_root_dacl(data_root: &Path) -> Result<(), String> {
    use std::ptr;
    use windows_sys::Win32::Foundation::LocalFree;
    use windows_sys::Win32::Security::Authorization::{
        ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1,
    };
    // windows-sys 0.59 では SetFileSecurityW・DACL_SECURITY_INFORMATION ともに
    // Win32::Security 直下（feature "Win32_Security"・既存有効）。advapi32.dll の link! 宣言。
    use windows_sys::Win32::Security::{SetFileSecurityW, DACL_SECURITY_INFORMATION};

    // P=継承保護, OICI=オブジェクト/コンテナ継承, FA=フルアクセス, OW=所有者, SY=LocalSystem。
    const DATA_ROOT_SDDL: &str = "D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)";

    // SDDL → self-relative セキュリティ記述子（LocalAlloc される）。
    let sddl_w = crate::util::to_wide(DATA_ROOT_SDDL);
    let mut psd: *mut std::ffi::c_void = ptr::null_mut();
    // SAFETY: sddl_w は NUL 終端。psd は成功時に LocalAlloc されたディスクリプタを受ける。
    let ok = unsafe {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl_w.as_ptr(),
            SDDL_REVISION_1,
            &mut psd,
            ptr::null_mut(),
        )
    };
    if ok == 0 || psd.is_null() {
        return Err("セキュリティ記述子の構築に失敗（DACL 未適用・起動は継続）".into());
    }

    // データルートのパスを NUL 終端 UTF-16 へ。
    let path_w = crate::util::to_wide(data_root);
    // DACL のみ差し替える（所有者/SACL は触らない）。
    // SAFETY: path_w は NUL 終端。psd は直前で得た有効な self-relative SD。
    let applied = unsafe { SetFileSecurityW(path_w.as_ptr(), DACL_SECURITY_INFORMATION, psd) };
    // SAFETY: psd は ConvertStringSecurityDescriptor... が確保した LocalAlloc。成否に関わらず解放する。
    unsafe {
        LocalFree(psd as _);
    }
    if applied == 0 {
        return Err("データルートへの DACL 適用に失敗（権限/非NTFS 等・起動は継続）".into());
    }
    Ok(())
}

/// 非 Windows: DACL は no-op（named pipe DACL 同様 Windows 固有）。
#[cfg(not(windows))]
pub fn harden_data_root_dacl(_data_root: &Path) -> Result<(), String> {
    Ok(())
}
