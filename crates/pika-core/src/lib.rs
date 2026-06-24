//! pika コアサービス層（UI/Tauri/wry 非依存）。
//!
//! design doc 4章のモジュール対応に従い、各機能スプリントで
//! document/workspace/watcher/diff/snapshot/render/search/settings/platform を写してきた。
//! sprint 7（a11y/エッジケース/配布の仕上げ）では UI に依存しない決定論部分として
//! 通知バーキュー運用（[`notify_queue`]）・診断ログ方針（[`diagnostic`]）・非テキスト/FSエッジ縮退
//! （[`nontext`]）・ビュー別5状態（[`view_state`]）・主要ショートカット表（[`shortcuts`]）を追加する。

pub mod cli;
pub mod data_root;
pub mod diagnostic;
pub mod diff;
pub mod encoding;
pub mod error;
pub mod hashing;
pub mod huge;
pub mod ipc;
pub mod nontext;
pub mod notify_queue;
pub mod path_verify;
pub mod range;
pub mod recent;
pub mod render;
pub mod review;
pub mod search;
pub mod settings;
pub mod shortcuts;
pub mod snapshot;
pub mod state;
pub mod view_state;
pub mod watcher;

pub use error::{PikaError, Result};
