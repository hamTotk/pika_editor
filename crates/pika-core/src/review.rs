//! 確認済み/巻き戻しフロー（要件8.3・design doc 11章・最上位原則「データを失わない」）。
//!
//! 本モジュールは UI/Tauri/wry/FS を一切知らない純粋ロジック（cargo test の決定論ゲート対象）。
//! 「確認済みにする」「すべて確認済みにする」「確認済み時点に戻す」の **意思決定**を担い、
//! 実際の object 保存・ベースライン更新・FS 読み書きは呼び出し側が結果（[`ReviewDecision`]）に従って行う。
//!
//! 設計の核（最上位原則1）:
//! - 確認済み確定の **直前に mtime/ハッシュを再照合**し、変化していれば中断して再差分を促す
//!   （ユーザーが見ていない内容をベースライン化しない＝要件8.3）。
//! - 「すべて確認済み」は **実行開始時点の未読集合をフリーズ**し、更新前ベースラインを
//!   `baseline-replace` で一括退避する（要件8.3）。
//! - 退避が物理的に取れない対象（10MB以上・画像＝ハッシュのみ）への破壊的操作は **退避不能ガード**で
//!   既定ブロック（要件7.3）。
//! - 退避（object 保存）に失敗したら **ベースラインを進めず**未読を維持し Result で失敗を返す
//!   （退避を握り潰さない＝データを失わない）。

use crate::snapshot::object::StashKind;
use crate::snapshot::policy::BaselinePolicy;

/// 確認済み確定時の現ディスク状態（再照合の入力）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiskState {
    /// 現ディスク内容の mtime（ミリ秒）。
    pub mtime_ms: u64,
    /// 現ディスク内容の LF 正規化ハッシュ。
    pub content_hash: String,
}

/// 差分計算に使ったスナップショット時点の状態（確認済みの基準）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiffSnapshot {
    /// 差分計算時点に読んだディスク内容の mtime（ミリ秒）。
    pub mtime_ms: u64,
    /// 差分計算時点に読んだディスク内容の LF 正規化ハッシュ。
    pub content_hash: String,
}

/// 「確認済みにする」の判定結果（呼び出し側が従う指示）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConfirmDecision {
    /// ベースラインを差分時点の内容（`content_hash`）へ更新してよい（未読解除）。
    UpdateBaseline {
        /// 新ベースラインの内容ハッシュ。
        content_hash: String,
        /// 内容 object を保存するか（StoreContent なら true・HashOnly なら false）。
        store_content: bool,
    },
    /// ディスクが変化していた。確認を中断し再差分を促す（ベースライン化しない＝要件8.3）。
    AbortReDiff,
}

/// 確認済み操作のエラー（最上位原則: 失敗を握り潰さずベースラインを進めない）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ReviewError {
    /// 退避（object 保存）に失敗。ベースラインは未更新（未読維持）。
    StashFailed(String),
    /// 退避不能対象（10MB以上・画像）への破壊的操作を既定ブロック（要件7.3）。
    StashImpossibleBlocked(String),
}

impl std::fmt::Display for ReviewError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ReviewError::StashFailed(m) => {
                write!(f, "退避に失敗したためベースラインを更新しませんでした: {m}")
            }
            ReviewError::StashImpossibleBlocked(m) => {
                write!(f, "退避が取れないためブロックしました: {m}")
            }
        }
    }
}

impl std::error::Error for ReviewError {}

/// 「確認済みにする」を判定する（要件8.3）。
///
/// 確定直前に差分時点（`diff_snapshot`）と現ディスク（`disk`）を再照合する。
/// 変化していれば [`ConfirmDecision::AbortReDiff`]（ユーザーが見ていない内容をベースライン化しない）。
/// 一致していればベースラインを差分時点の内容へ更新してよい。
pub fn decide_confirm(
    diff_snapshot: &DiffSnapshot,
    disk: &DiskState,
    policy: BaselinePolicy,
) -> ConfirmDecision {
    // mtime かハッシュのどちらかでも変化していれば中断（mtime 据え置きの取りこぼし対策にハッシュも見る）。
    if diff_snapshot.mtime_ms != disk.mtime_ms || diff_snapshot.content_hash != disk.content_hash {
        return ConfirmDecision::AbortReDiff;
    }
    ConfirmDecision::UpdateBaseline {
        content_hash: diff_snapshot.content_hash.clone(),
        store_content: policy.stores_content(),
    }
}

/// 1 ファイル分の「すべて確認済み」対象（フリーズした未読集合の1件）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConfirmAllTarget {
    /// 相対パス。
    pub rel_path: String,
    /// フリーズ時点の差分スナップショット（基準）。
    pub frozen: DiffSnapshot,
    /// 確定直前の現ディスク状態。
    pub disk: DiskState,
    /// このファイルの更新前ベースラインの内容 object ハッシュ（退避対象。内容を持つ時のみ）。
    pub prev_baseline_object: Option<String>,
    /// ベースライン保存方針（HashOnly なら退避対象 object 無し）。
    pub policy: BaselinePolicy,
}

/// 「すべて確認済み」1 件の処理結果（呼び出し側が従う指示）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConfirmAllOutcome {
    /// ベースライン更新可（更新前 object を baseline-replace バッチへ退避してから）。
    Updated {
        /// 相対パス。
        rel_path: String,
        /// 新ベースライン内容ハッシュ。
        content_hash: String,
        /// baseline-replace バッチへ退避すべき更新前 object（内容を持つ時のみ）。
        stash_object: Option<String>,
        /// 内容 object を保存するか。
        store_content: bool,
    },
    /// 処理中に内容が変化したためスキップ（未読のまま残す＝要件8.3）。
    SkippedChanged {
        /// 相対パス。
        rel_path: String,
    },
}

/// 「すべて確認済みにする」の一括判定（要件8.3）。
///
/// 実行開始時点でフリーズした未読集合 `targets` を順に処理する。
/// - 処理中に内容が変化したファイルは確認をスキップし未読のまま残す（並行書込で未確認内容を
///   ベースライン化しない）。
/// - 変化していないファイルはベースライン更新可とし、更新前ベースライン object を
///   `baseline-replace` バッチへ退避する（ワンクリックで一括取り消し可能＝要件8.3）。
///
/// 退避結合の失敗は呼び出し側が個別に検出してベースラインを進めない（[`decide_confirm`] と同方針）。
pub fn decide_confirm_all(targets: &[ConfirmAllTarget]) -> Vec<ConfirmAllOutcome> {
    targets
        .iter()
        .map(|t| {
            if t.frozen.mtime_ms != t.disk.mtime_ms || t.frozen.content_hash != t.disk.content_hash
            {
                ConfirmAllOutcome::SkippedChanged {
                    rel_path: t.rel_path.clone(),
                }
            } else {
                ConfirmAllOutcome::Updated {
                    rel_path: t.rel_path.clone(),
                    content_hash: t.frozen.content_hash.clone(),
                    // 内容を持つベースラインのみ baseline-replace 退避（HashOnly は退避対象 object 無し）。
                    stash_object: if t.policy.stores_content() {
                        t.prev_baseline_object.clone()
                    } else {
                        None
                    },
                    store_content: t.policy.stores_content(),
                }
            }
        })
        .collect()
}

/// 「確認済み時点に戻す」（ファイル単位巻き戻し）の判定（要件8.3・7.3 退避不能ガード）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RollbackDecision {
    /// 巻き戻してよい。現在内容を `rollback` 退避してからベースライン内容で上書きする。
    Rollback {
        /// 退避種別（常に Rollback。呼び出し側が object メタへ書く）。
        stash_kind: StashKind,
    },
}

/// 「確認済み時点に戻す」を判定する（要件8.3・7.3）。
///
/// - ベースラインが内容を持たない（HashOnly＝10MB以上・画像）場合、巻き戻し非対象＝退避不能ガードで
///   ブロックする（要件7.3 既定ブロック）。
/// - 現在内容が退避不能（`current_storable=false`＝10MB以上・画像）なら、巻き戻しで失われる現在内容を
///   退避できないためブロックする（退避が最後の砦・要件7.3）。
/// - いずれも満たせば現在内容を rollback 退避してからベースライン内容で上書きしてよい。
pub fn decide_rollback(
    baseline_has_content: bool,
    current_storable: bool,
) -> Result<RollbackDecision, ReviewError> {
    if !baseline_has_content {
        return Err(ReviewError::StashImpossibleBlocked(
            "ベースラインが内容を持たない（10MB以上/画像）ため巻き戻せません".into(),
        ));
    }
    if !current_storable {
        return Err(ReviewError::StashImpossibleBlocked(
            "現在内容を退避できない（10MB以上/画像）ため巻き戻しをブロックしました".into(),
        ));
    }
    Ok(RollbackDecision::Rollback {
        stash_kind: StashKind::Rollback,
    })
}

/// 破壊的操作（取り込み・上書き保存・確認済み時点に戻す）の退避不能ガード（要件7.3）。
///
/// 退避が物理的に取れない対象（10MB以上・画像＝内容を保存しない方針）への破壊的操作は
/// **既定でブロック**する。`allow_unstorable=true`（設定「退避不能時は強い確認のうえ許可」）なら通す。
pub fn guard_destructive(storable: bool, allow_unstorable: bool) -> Result<(), ReviewError> {
    if storable || allow_unstorable {
        Ok(())
    } else {
        Err(ReviewError::StashImpossibleBlocked(
            "退避不能な対象への破壊的操作を既定でブロックしました".into(),
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn snap(mtime: u64, hash: &str) -> DiffSnapshot {
        DiffSnapshot {
            mtime_ms: mtime,
            content_hash: hash.into(),
        }
    }
    fn disk(mtime: u64, hash: &str) -> DiskState {
        DiskState {
            mtime_ms: mtime,
            content_hash: hash.into(),
        }
    }

    #[test]
    fn 一致なら確認済みでベースライン更新() {
        let d = decide_confirm(
            &snap(100, "h"),
            &disk(100, "h"),
            BaselinePolicy::StoreContent,
        );
        assert_eq!(
            d,
            ConfirmDecision::UpdateBaseline {
                content_hash: "h".into(),
                store_content: true
            }
        );
    }

    #[test]
    fn 確定直前にディスクが変化していたら中断して再差分() {
        // ハッシュが変わっていれば中断（ユーザーが見ていない内容をベースライン化しない＝要件8.3）。
        let d = decide_confirm(
            &snap(100, "h"),
            &disk(100, "h-changed"),
            BaselinePolicy::StoreContent,
        );
        assert_eq!(d, ConfirmDecision::AbortReDiff);
    }

    #[test]
    fn mtime_だけ変化でも中断する() {
        let d = decide_confirm(
            &snap(100, "h"),
            &disk(200, "h"),
            BaselinePolicy::StoreContent,
        );
        assert_eq!(d, ConfirmDecision::AbortReDiff);
    }

    #[test]
    fn ハッシュのみ方針は内容保存しないベースライン更新() {
        let d = decide_confirm(&snap(1, "h"), &disk(1, "h"), BaselinePolicy::HashOnly);
        match d {
            ConfirmDecision::UpdateBaseline { store_content, .. } => assert!(!store_content),
            _ => panic!("更新になるはず"),
        }
    }

    #[test]
    fn すべて確認済みは変化ファイルをスキップし他は更新() {
        let targets = vec![
            ConfirmAllTarget {
                rel_path: "ok.md".into(),
                frozen: snap(1, "h1"),
                disk: disk(1, "h1"),
                prev_baseline_object: Some("old-1".into()),
                policy: BaselinePolicy::StoreContent,
            },
            ConfirmAllTarget {
                rel_path: "changed.md".into(),
                frozen: snap(1, "h2"),
                disk: disk(2, "h2-new"), // 処理中に変化。
                prev_baseline_object: Some("old-2".into()),
                policy: BaselinePolicy::StoreContent,
            },
        ];
        let outcomes = decide_confirm_all(&targets);
        assert_eq!(
            outcomes[0],
            ConfirmAllOutcome::Updated {
                rel_path: "ok.md".into(),
                content_hash: "h1".into(),
                stash_object: Some("old-1".into()),
                store_content: true,
            }
        );
        assert_eq!(
            outcomes[1],
            ConfirmAllOutcome::SkippedChanged {
                rel_path: "changed.md".into()
            }
        );
    }

    #[test]
    fn すべて確認済みのハッシュのみ方針は退避_object_無し() {
        let targets = vec![ConfirmAllTarget {
            rel_path: "big.json".into(),
            frozen: snap(1, "h"),
            disk: disk(1, "h"),
            prev_baseline_object: None,
            policy: BaselinePolicy::HashOnly,
        }];
        let outcomes = decide_confirm_all(&targets);
        match &outcomes[0] {
            ConfirmAllOutcome::Updated {
                stash_object,
                store_content,
                ..
            } => {
                assert!(stash_object.is_none());
                assert!(!store_content);
            }
            _ => panic!("更新になるはず"),
        }
    }

    #[test]
    fn 巻き戻しは内容ありかつ現在退避可能なら許可() {
        let d = decide_rollback(true, true).unwrap();
        assert_eq!(
            d,
            RollbackDecision::Rollback {
                stash_kind: StashKind::Rollback
            }
        );
    }

    #[test]
    fn ベースラインがハッシュのみなら巻き戻しブロック() {
        // 10MB以上/画像＝退避不能ガード（要件7.3）。
        let e = decide_rollback(false, true).unwrap_err();
        assert!(matches!(e, ReviewError::StashImpossibleBlocked(_)));
    }

    #[test]
    fn 現在内容が退避不能なら巻き戻しブロック() {
        let e = decide_rollback(true, false).unwrap_err();
        assert!(matches!(e, ReviewError::StashImpossibleBlocked(_)));
    }

    #[test]
    fn 退避不能ガードは既定ブロックし許可フラグで通す() {
        // 退避不能（storable=false）かつ allow=false → ブロック（要件7.3 既定ブロック）。
        assert!(guard_destructive(false, false).is_err());
        // storable=true は常に通る。
        assert!(guard_destructive(true, false).is_ok());
        // 設定で許可すれば退避不能でも通す。
        assert!(guard_destructive(false, true).is_ok());
    }
}
