//! pika コアサービス層（UI/Tauri/wry 非依存）。
//!
//! design doc 4章のモジュール対応に従い、各機能スプリントで
//! document/workspace/watcher/diff/snapshot/render/search/settings/platform を追加していく。
//! 本スプリント（sprint 1）では最薄ループの土台として CLI 引数パースと
//! データルート解決の純粋ロジックのみを置き、cargo test の決定論ゲートを立てる。

pub mod cli;
pub mod data_root;
pub mod diff;
pub mod error;
pub mod render;
pub mod review;
pub mod snapshot;
pub mod watcher;

pub use error::{PikaError, Result};
