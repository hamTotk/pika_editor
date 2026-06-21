//! スナップショット（ベースライン・退避・content-addressed object・容量管理・要件9・design doc 11章）。
//!
//! 本モジュールは UI/Tauri/wry を一切知らない純粋ロジック（cargo test の決定論ゲート対象）。
//! 実際の FS 書込・DACL 設定・zstd 圧縮の永続化は呼び出し側（`src-tauri`）が行い、
//! ここには「索引（index）＋ object 台帳の決定論モデル」と「容量管理・参照計数・index 破損復元」を集約する
//! （最上位原則「データを失わない」のコア＝退避が最後の砦）。
//!
//! モジュール構成:
//! - [`object`] — content-addressed object（twox-hash 重複排除）・自己記述メタ・zstd 往復・LF 正規化ハッシュ。
//! - [`store`] — ベースライン/退避の索引・参照計数・index 破損からの退避一覧再生成。
//! - [`gc`] — 容量管理（ファイルごと最新10件LRU＋14日保護・全体500MB＋90日GC・共有object全参照確認後削除）。
//! - [`policy`] — 機密ファイル/10MB境界の「ハッシュのみ記録」判定（差分・巻き戻し非対象）。

pub mod gc;
pub mod object;
pub mod policy;
pub mod store;

pub use gc::{plan_gc, GcConfig, GcPlan};
pub use object::{
    hash_normalized, zstd_compress, zstd_decompress, ObjectMeta, StashKind, DEFAULT_ZSTD_LEVEL,
};
pub use policy::{baseline_policy, BaselinePolicy, DEFAULT_CONTENT_LIMIT_BYTES};
pub use store::{
    BaselineRef, SnapshotError, SnapshotStore, StashEntry, StashResult, MAX_STASH_PER_FILE,
};
