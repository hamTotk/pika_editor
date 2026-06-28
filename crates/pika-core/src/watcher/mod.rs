//! watcher 自前合成層（要件7.1/7.4・4.2・design doc 4章/19章）。
//!
//! 本モジュールは **OS 監視 API（notify / ReadDirectoryChangesW）を一切知らない**。
//! 監視スレッド（`src-tauri` 側）が raw event を [`RawFsEvent`] へ写し、本層へ流し込む。
//! 本層は I/O を行わない純粋ロジックに徹し（cargo test の決定論ゲート対象）、
//! デバウンス/合体・確定読み判定・自己保存抑制・rename 正規化・オーバーフロー再同期を担う。
//!
//! 設計原則（CLAUDE.md / design doc 3章）:
//! - **固まらない** — 監視スレッドは raw event をキューに積むだけ。合成は本層が軽量に行う。
//! - **コアは UI を知らない** — Tauri/wry/notify 非依存。橋渡しは `src-tauri` の監視スレッドのみ。
//!
//! モジュール構成（design doc 4章 watcher の責務をサブモジュールへ分解）:
//! - [`event`] — 抽象 raw event 型・[`FsChange`]（合成結果）・[`FileId`]（rename 補強）
//! - [`debounce`] — デバウンス/合体・静穏期間＋mtime/サイズ安定での確定読み判定
//! - [`self_save`] — 自己保存抑制（ハッシュ一致主条件・時刻窓内は複数イベントを抑制・GC は補助安全弁）
//! - [`rename`] — rename 旧名/新名ペア正規化（FileId 補強・スワップ/往復/上書き/片側欠落）
//! - [`overflow`] — オーバーフロー再同期（全再列挙→mtime/サイズ→ハッシュ比較）
//! - [`temp`] — pika 一時ファイル（`*.pika.tmp`）判定（監視・列挙からの除外＝自己保存抑制の前段）

pub mod debounce;
pub mod event;
pub mod overflow;
pub mod rename;
pub mod self_save;
pub mod temp;

pub use debounce::{Debouncer, PendingState};
pub use event::{FileId, FsChange, FsChangeKind, RawFsEvent, RawFsEventKind};
pub use overflow::{resync_against_baseline, FileFingerprint, ResyncOutcome};
pub use rename::{normalize_renames, RenameResolution};
pub use self_save::{SaveToken, SaveTokenStore, SuppressDecision};
pub use temp::is_pika_temp;
