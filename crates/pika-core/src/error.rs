//! コア公開 API のエラー型（design doc 12章「コア公開 API は Result<T, PikaError>」）。
//! 例外はモジュール内部に閉じ、境界では必ず `Result` を返す。
//!
//! Display 文言は `thiserror` の `#[error("...")]` 属性で宣言する（canon 指定・手書き Display 廃止）。
//! 文言は手書き版と 1 バイトも変えていない（`{0}` はタプル要素 0＝旧 `{msg}` と同一）。

/// pika コアのエラー。機能スプリントで variant を増やしていく。
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum PikaError {
    /// 引数・入力の検査に失敗（CLI 引数・パス検証など）。
    #[error("不正な引数: {0}")]
    InvalidArgument(String),
    /// データルート/パスの解決に失敗。
    #[error("パス解決に失敗: {0}")]
    PathResolution(String),
}

/// コア公開 API の戻り型。
pub type Result<T> = std::result::Result<T, PikaError>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn display文言は手書き版と一致する() {
        // thiserror 採用後も Display 出力が 1 バイトも変わらないことを固定する（文言回帰防止）。
        assert_eq!(
            PikaError::InvalidArgument("x".into()).to_string(),
            "不正な引数: x"
        );
        assert_eq!(
            PikaError::PathResolution("y".into()).to_string(),
            "パス解決に失敗: y"
        );
    }
}
