//! コア公開 API のエラー型（design doc 12章「コア公開 API は Result<T, PikaError>」）。
//! 例外はモジュール内部に閉じ、境界では必ず `Result` を返す。

use std::fmt;

/// pika コアのエラー。機能スプリントで variant を増やしていく。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PikaError {
    /// 引数・入力の検査に失敗（CLI 引数・パス検証など）。
    InvalidArgument(String),
    /// データルート/パスの解決に失敗。
    PathResolution(String),
}

impl fmt::Display for PikaError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PikaError::InvalidArgument(msg) => write!(f, "不正な引数: {msg}"),
            PikaError::PathResolution(msg) => write!(f, "パス解決に失敗: {msg}"),
        }
    }
}

impl std::error::Error for PikaError {}

/// コア公開 API の戻り型。
pub type Result<T> = std::result::Result<T, PikaError>;
