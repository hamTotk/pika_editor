//! rename 旧名/新名ペア正規化（要件4.2・design doc 164/168行・19章 rename継承）。
//!
//! 監視 API は rename を「旧名側（From）」「新名側（To）」の 2 イベントで通知するが、
//! 順序や対応付けは保証されない。本層は時間窓内の From/To を**FileId 補強**でペア化し、
//! 未読・ベースライン・退避の引き継ぎ対象を**決定論的**に算定する。
//!
//! 正規化規則（要件4.2 の安全側）:
//! - **ペア成立**（同一 FileId の From↔To、または時間窓内で一意な From↔To）= rename 継承。
//! - **相互スワップ（A↔B）** = FileId で各々を追跡し 2 本の rename に正規化。
//! - **往復（A→B→A）** = 最終的に同一パスへ戻るため「変更なし」へ畳む（FileId が同一）。
//! - **クロスディレクトリ移動** = ペアが成立すれば rename 継承（パスの親が違っても FileId 主キー）。
//! - **上書き rename**（既存ファイルへ rename）= 移動先の旧エントリを上書き＝内容変更扱い。
//! - **片側欠落**（From 単独 = 削除 / To 単独 = 新規）= 安全側に倒す。
//!
//! 本層は I/O を行わない純粋ロジック（FileId/時刻は監視スレッドが採取して渡す）。

use crate::watcher::event::{FileId, FsChange, RawFsEvent, RawFsEventKind};

/// rename 正規化の 1 結果（引き継ぎ対象を決定論的に表す）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RenameResolution {
    /// ペア成立 = rename 継承（未読・ベースライン・退避を `from`→`to` へ引き継ぐ）。
    Renamed {
        /// 旧パス。
        from: String,
        /// 新パス。
        to: String,
        /// 上書き rename か（移動先に既存エントリがあった＝そのエントリは内容変更扱い）。
        overwrote_existing: bool,
    },
    /// From 単独（移動先が監視外/片側欠落）= 旧パスを削除扱い。
    RemovedSource {
        /// 削除扱いにする旧パス。
        path: String,
    },
    /// To 単独（移動元が監視外/片側欠落）= 新パスを新規扱い。
    CreatedTarget {
        /// 新規扱いにする新パス。
        path: String,
    },
    /// 往復で同一パスへ戻った = 実質変更なし（畳んで未読を付けない）。
    NoChange {
        /// 元に戻ったパス。
        path: String,
    },
}

impl RenameResolution {
    /// 合成結果（フロントへ送る変更）へ写す。`NoChange` は変更を出さない（`None`）。
    pub fn to_change(&self) -> Option<FsChange> {
        match self {
            RenameResolution::Renamed { from, to, .. } => Some(FsChange::renamed(from, to)),
            RenameResolution::RemovedSource { path } => Some(FsChange::removed(path)),
            RenameResolution::CreatedTarget { path } => Some(FsChange::created(path)),
            RenameResolution::NoChange { .. } => None,
        }
    }
}

/// 既定の rename ペア成立時間窓（ミリ秒）。窓内に揃わない From/To は片側欠落へ倒す。
pub const DEFAULT_RENAME_WINDOW_MS: u64 = 200;

/// 時間窓内の From/To raw event 群を正規化する。
///
/// 入力は `RenamedFrom` / `RenamedTo` のみ（呼び出し側がフィルタ済み）。
/// 出力は決定論的に並べた [`RenameResolution`] のリスト。
///
/// ペアリング戦略（要件4.2・design doc 168行）:
/// 1. **FileId が両側で取れるもの**を最優先でペア化（相互スワップ・上書き rename を正しく解く）。
/// 2. FileId が取れない From/To は、**時間窓内で一意なら**パスベースでペア化。
/// 3. ペアにならなかった From は削除・To は新規（片側欠落＝安全側）。
/// 4. ペアの旧パス＝新パスなら往復として `NoChange` に畳む。
pub fn normalize_renames(events: &[RawFsEvent]) -> Vec<RenameResolution> {
    let froms: Vec<&RawFsEvent> = events
        .iter()
        .filter(|e| e.kind == RawFsEventKind::RenamedFrom)
        .collect();
    let tos: Vec<&RawFsEvent> = events
        .iter()
        .filter(|e| e.kind == RawFsEventKind::RenamedTo)
        .collect();

    let mut from_used = vec![false; froms.len()];
    let mut to_used = vec![false; tos.len()];
    let mut out: Vec<RenameResolution> = Vec::new();

    // --- 段1: FileId 一致で確定ペア化（相互スワップ・上書き rename を解く）。
    for (fi, f) in froms.iter().enumerate() {
        let Some(fid) = f.file_id else { continue };
        if let Some((ti, t)) = find_matching_to(&tos, &to_used, |t| t.file_id == Some(fid)) {
            from_used[fi] = true;
            to_used[ti] = true;
            push_pair(&mut out, f, t);
        }
    }

    // --- 段2: FileId が片方でも欠ける残りを、時間窓内で一意ならパスでペア化。
    //          From を時刻順に見て、未使用の To の中から時間窓内のものを 1 件選ぶ。
    for (fi, f) in froms.iter().enumerate() {
        if from_used[fi] {
            continue;
        }
        if let Some((ti, t)) = find_matching_to(&tos, &to_used, |t| {
            // 片方でも FileId が欠ける場合のみパスベースのペア化対象。両側 FileId 有りは
            // 段1 で解決済みなので、ここでは時間窓内かどうかだけで一意ペアを決める。
            (f.file_id.is_none() || t.file_id.is_none()) && within_window(f.at_ms, t.at_ms)
        }) {
            from_used[fi] = true;
            to_used[ti] = true;
            push_pair(&mut out, f, t);
        }
    }

    // --- 段3: ペアにならなかった片側を安全側に倒す。
    for (fi, f) in froms.iter().enumerate() {
        if !from_used[fi] {
            out.push(RenameResolution::RemovedSource {
                path: f.path.clone(),
            });
        }
    }
    for (ti, t) in tos.iter().enumerate() {
        if !to_used[ti] {
            out.push(RenameResolution::CreatedTarget {
                path: t.path.clone(),
            });
        }
    }

    out
}

/// 述語に合致する未使用の To を 1 件返す（最初に見つかったもの＝決定論）。
fn find_matching_to<'a>(
    tos: &[&'a RawFsEvent],
    to_used: &[bool],
    pred: impl Fn(&RawFsEvent) -> bool,
) -> Option<(usize, &'a RawFsEvent)> {
    tos.iter()
        .enumerate()
        .find(|(ti, t)| !to_used[*ti] && pred(t))
        .map(|(ti, t)| (ti, *t))
}

/// 成立したペアを `RenameResolution` へ写して push する。
///
/// - 旧パス＝新パス（往復で戻った）なら `NoChange`。
/// - 上書き rename（移動先に既存エントリがある）の判定は、本層がディスクを見ないため
///   引き継ぎを受ける WorkspaceController 側で「移動先 relPath の既存台帳エントリ有無」で
///   確定する（要件4.2「リネーム先に既存エントリがある場合は移動元で上書き」）。
///   本層では `overwrote_existing=false` を返し、上書き確定は上位層に委ねる。
fn push_pair(out: &mut Vec<RenameResolution>, from: &RawFsEvent, to: &RawFsEvent) {
    if from.path == to.path {
        out.push(RenameResolution::NoChange {
            path: to.path.clone(),
        });
        return;
    }
    out.push(RenameResolution::Renamed {
        from: from.path.clone(),
        to: to.path.clone(),
        overwrote_existing: false,
    });
}

/// `at_a` と `at_b` が rename ペア成立窓内か。
fn within_window(at_a: u64, at_b: u64) -> bool {
    let d = at_a.max(at_b) - at_a.min(at_b);
    d <= DEFAULT_RENAME_WINDOW_MS
}

/// FileId を作る補助（テスト/監視スレッド用）。
pub fn file_id(volume: u64, index: u64) -> FileId {
    FileId { volume, index }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn from(path: &str, at_ms: u64, id: Option<FileId>) -> RawFsEvent {
        RawFsEvent {
            kind: RawFsEventKind::RenamedFrom,
            path: path.into(),
            at_ms,
            file_id: id,
            mtime_ms: None,
            size: None,
        }
    }
    fn to(path: &str, at_ms: u64, id: Option<FileId>) -> RawFsEvent {
        RawFsEvent {
            kind: RawFsEventKind::RenamedTo,
            path: path.into(),
            at_ms,
            file_id: id,
            mtime_ms: None,
            size: None,
        }
    }

    #[test]
    fn fileid_一致で単純な_rename_を継承する() {
        let id = file_id(1, 100);
        let evs = vec![from("a.md", 0, Some(id)), to("b.md", 10, Some(id))];
        let r = normalize_renames(&evs);
        assert_eq!(
            r,
            vec![RenameResolution::Renamed {
                from: "a.md".into(),
                to: "b.md".into(),
                overwrote_existing: false,
            }]
        );
    }

    #[test]
    fn 相互スワップ_a_b_を_2_本の_rename_に正規化する() {
        // A↔B: A(id1)→tmp名(B), B(id2)→A名 のように FileId で各々を追える。
        let id_a = file_id(1, 1);
        let id_b = file_id(1, 2);
        let evs = vec![
            from("A.md", 0, Some(id_a)),
            to("B.md", 5, Some(id_a)), // id_a は A→B へ
            from("B.md", 1, Some(id_b)),
            to("A.md", 6, Some(id_b)), // id_b は B→A へ
        ];
        let r = normalize_renames(&evs);
        assert!(r.contains(&RenameResolution::Renamed {
            from: "A.md".into(),
            to: "B.md".into(),
            overwrote_existing: false,
        }));
        assert!(r.contains(&RenameResolution::Renamed {
            from: "B.md".into(),
            to: "A.md".into(),
            overwrote_existing: false,
        }));
        assert_eq!(r.len(), 2);
    }

    #[test]
    fn 往復_a_b_a_は変更なしへ畳む() {
        // 同一 FileId が最終的に元のパスへ戻る（From=A, To=A）。
        let id = file_id(1, 7);
        let evs = vec![from("note.md", 0, Some(id)), to("note.md", 12, Some(id))];
        let r = normalize_renames(&evs);
        assert_eq!(
            r,
            vec![RenameResolution::NoChange {
                path: "note.md".into()
            }]
        );
        // NoChange は合成結果を出さない。
        assert!(r[0].to_change().is_none());
    }

    #[test]
    fn クロスディレクトリ移動も_fileid_でペア化する() {
        let id = file_id(1, 9);
        let evs = vec![
            from(r"src\a.md", 0, Some(id)),
            to(r"dst\sub\a.md", 8, Some(id)),
        ];
        let r = normalize_renames(&evs);
        assert_eq!(
            r,
            vec![RenameResolution::Renamed {
                from: r"src\a.md".into(),
                to: r"dst\sub\a.md".into(),
                overwrote_existing: false,
            }]
        );
    }

    #[test]
    fn from_単独は削除へ倒す() {
        let evs = vec![from("moved-out.md", 0, Some(file_id(1, 3)))];
        let r = normalize_renames(&evs);
        assert_eq!(
            r,
            vec![RenameResolution::RemovedSource {
                path: "moved-out.md".into()
            }]
        );
        assert_eq!(r[0].to_change(), Some(FsChange::removed("moved-out.md")));
    }

    #[test]
    fn to_単独は新規へ倒す() {
        let evs = vec![to("moved-in.md", 0, Some(file_id(1, 4)))];
        let r = normalize_renames(&evs);
        assert_eq!(
            r,
            vec![RenameResolution::CreatedTarget {
                path: "moved-in.md".into()
            }]
        );
        assert_eq!(r[0].to_change(), Some(FsChange::created("moved-in.md")));
    }

    #[test]
    fn fileid_なしでも時間窓内で一意ならパスでペア化する() {
        // FS が FileId を出さない場合、時間窓内で一意な From/To をペア化する。
        let evs = vec![from("a.md", 100, None), to("b.md", 150, None)];
        let r = normalize_renames(&evs);
        assert_eq!(
            r,
            vec![RenameResolution::Renamed {
                from: "a.md".into(),
                to: "b.md".into(),
                overwrote_existing: false,
            }]
        );
    }

    #[test]
    fn fileid_なしで時間窓を超えたら片側欠落へ倒す() {
        // 窓（200ms）を超えた From/To はペア化せず、削除＋新規へ安全側に倒す。
        let evs = vec![from("a.md", 0, None), to("b.md", 1000, None)];
        let r = normalize_renames(&evs);
        assert!(r.contains(&RenameResolution::RemovedSource {
            path: "a.md".into()
        }));
        assert!(r.contains(&RenameResolution::CreatedTarget {
            path: "b.md".into()
        }));
        assert_eq!(r.len(), 2);
    }
}
