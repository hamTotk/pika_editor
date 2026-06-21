//! デバウンス/合体・確定読み判定（要件7.1・design doc 165行「確定読み」）。
//!
//! 役割:
//! - 同一パスへの連続イベントを 1 件に**合体**する（エージェントのストリーム書き込み対策）。
//! - 直近変更から**静穏期間**（既定100ms）待ち、かつ **mtime/サイズが連続イベント間で安定**した
//!   ことを確認してから「確定」とする（共有モード書き込みの末尾欠損＝中途内容を読まない）。
//!
//! 本モジュールは時刻を引数で受ける純粋ロジック（タイマー実体は監視スレッドが持つ）。
//! `feed` で raw event を積み、`drain_settled(now_ms)` で確定したパスの合成結果を取り出す。

use crate::watcher::event::{FsChange, RawFsEvent, RawFsEventKind};
use std::collections::BTreeMap;

/// 既定のデバウンス静穏期間（ミリ秒。要件7.1「目安100ms」）。
pub const DEFAULT_DEBOUNCE_MS: u64 = 100;

/// 1 パス分の保留状態（合体中のイベント）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PendingState {
    /// 合体後の実効種別（Created/Modified/Removed/rename 片側）。
    kind: RawFsEventKind,
    /// 最後にイベントを受けた時刻（静穏期間の起点）。
    last_at_ms: u64,
    /// 直近 2 回の (mtime, size)。安定確認に使う（同値が連続したら安定）。
    last_meta: Option<(Option<u64>, Option<u64>)>,
    prev_meta: Option<(Option<u64>, Option<u64>)>,
    /// メタを 1 度でも観測したか（メタ取得不能 FS では時間のみで確定する）。
    meta_seen: bool,
}

/// パス単位のデバウンサ。rename とオーバーフローは別モジュールが扱い、ここは
/// Created/Modified/Removed の合体・確定読みに専念する。
#[derive(Debug, Default)]
pub struct Debouncer {
    debounce_ms: u64,
    pending: BTreeMap<String, PendingState>,
}

impl Debouncer {
    /// 既定の静穏期間でデバウンサを作る。
    pub fn new() -> Self {
        Self::with_debounce(DEFAULT_DEBOUNCE_MS)
    }

    /// 静穏期間を指定してデバウンサを作る（設定可・要件7.1）。
    pub fn with_debounce(debounce_ms: u64) -> Self {
        Self {
            debounce_ms,
            pending: BTreeMap::new(),
        }
    }

    /// 保留中のパス数（テスト/診断用）。
    pub fn pending_len(&self) -> usize {
        self.pending.len()
    }

    /// raw event を 1 件積む（rename/overflow 以外）。同一パスは合体する。
    ///
    /// 合体規則（要件4.2 の安全側）:
    /// - Created→…→Modified は **Created** のまま（新規作成の途中書き）。
    /// - …→Removed は **Removed**（最後に消えたら削除）。
    /// - Removed→Created は **Modified**（消えて作り直し＝内容変更扱いに倒す）。
    pub fn feed(&mut self, ev: &RawFsEvent) {
        debug_assert!(
            !matches!(
                ev.kind,
                RawFsEventKind::RenamedFrom | RawFsEventKind::RenamedTo | RawFsEventKind::Overflow
            ),
            "rename/overflow は専用モジュールで処理する"
        );

        let meta = (ev.mtime_ms, ev.size);
        let meta_present = ev.mtime_ms.is_some() || ev.size.is_some();

        match self.pending.get_mut(&ev.path) {
            Some(state) => {
                state.kind = coalesce_kind(&state.kind, &ev.kind);
                state.last_at_ms = ev.at_ms;
                if meta_present {
                    state.prev_meta = state.last_meta.take();
                    state.last_meta = Some(meta);
                    state.meta_seen = true;
                }
            }
            None => {
                self.pending.insert(
                    ev.path.clone(),
                    PendingState {
                        kind: ev.kind.clone(),
                        last_at_ms: ev.at_ms,
                        last_meta: if meta_present { Some(meta) } else { None },
                        prev_meta: None,
                        meta_seen: meta_present,
                    },
                );
            }
        }
    }

    /// `now_ms` 時点で**確定した**パスの合成結果を取り出し、保留集合から除く。
    ///
    /// 確定条件（要件7.1「確定読み」）:
    /// 1. 最後のイベントから静穏期間（`debounce_ms`）が経過している。
    /// 2. かつ次のいずれか:
    ///    - 削除イベントである（削除は安定確認不要）。
    ///    - メタを観測していない FS（取得不能）である（時間のみで確定）。
    ///    - 直近 2 回の (mtime, size) が**同値で安定**している（中途内容でない）。
    ///
    /// メタは観測したが安定確認が 1 回ぶんしか取れていない（prev が無い）場合は、
    /// 安定とみなさず次の `drain` まで保留する（末尾欠損の防止＝データを失わない）。
    pub fn drain_settled(&mut self, now_ms: u64) -> Vec<FsChange> {
        let ready: Vec<String> = self
            .pending
            .iter()
            .filter(|(_, st)| self.is_settled(st, now_ms))
            .map(|(p, _)| p.clone())
            .collect();

        let mut out = Vec::with_capacity(ready.len());
        for path in ready {
            if let Some(st) = self.pending.remove(&path) {
                out.push(match st.kind {
                    RawFsEventKind::Created => FsChange::created(path),
                    RawFsEventKind::Removed => FsChange::removed(path),
                    // Modified および合体不能の残りは内容変更に倒す（安全側）。
                    _ => FsChange::modified(path),
                });
            }
        }
        out
    }

    fn is_settled(&self, st: &PendingState, now_ms: u64) -> bool {
        // 静穏期間が経過していなければ未確定（連続書き込み中）。
        if now_ms.saturating_sub(st.last_at_ms) < self.debounce_ms {
            return false;
        }
        // 削除は安定確認不要（消えたものは中途内容を持たない）。
        if matches!(st.kind, RawFsEventKind::Removed) {
            return true;
        }
        // メタを一切観測できない FS は時間のみで確定する（縮退せず維持）。
        if !st.meta_seen {
            return true;
        }
        // メタを観測したなら、直近 2 回が同値で安定したことを要求する（中途内容を読まない）。
        match (st.prev_meta, st.last_meta) {
            (Some(prev), Some(last)) => prev == last,
            // 安定確認が 1 回ぶんしか無い間は保留（次の drain まで待つ）。
            _ => false,
        }
    }
}

/// 同一パスへの連続イベント種別を合体する（要件4.2 の安全側）。
fn coalesce_kind(existing: &RawFsEventKind, incoming: &RawFsEventKind) -> RawFsEventKind {
    use RawFsEventKind::*;
    match (existing, incoming) {
        // 削除が最後に来たら削除。
        (_, Removed) => Removed,
        // 削除のあとに作成/変更が来たら「消えて作り直し」＝内容変更に倒す。
        (Removed, Created) | (Removed, Modified) => Modified,
        // 新規作成の途中書き（Created→Modified）は Created のまま。
        (Created, Modified) | (Created, Created) => Created,
        // それ以外は最新の種別を採用。
        (_, k) => k.clone(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::watcher::event::RawFsEvent;

    fn ev(path: &str, kind: RawFsEventKind, at_ms: u64, mtime: u64, size: u64) -> RawFsEvent {
        RawFsEvent {
            kind,
            path: path.into(),
            at_ms,
            file_id: None,
            mtime_ms: Some(mtime),
            size: Some(size),
        }
    }

    fn ev_no_meta(path: &str, kind: RawFsEventKind, at_ms: u64) -> RawFsEvent {
        RawFsEvent {
            kind,
            path: path.into(),
            at_ms,
            file_id: None,
            mtime_ms: None,
            size: None,
        }
    }

    #[test]
    fn 連続イベントが_1_件に合成される() {
        // 同一パスへ 3 回の Modified（ストリーム書き込み）→ 確定後 1 件だけ出る。
        let mut d = Debouncer::with_debounce(100);
        d.feed(&ev("a.md", RawFsEventKind::Modified, 0, 10, 100));
        d.feed(&ev("a.md", RawFsEventKind::Modified, 30, 11, 200));
        // 安定（同値）の 2 連続を作る。
        d.feed(&ev("a.md", RawFsEventKind::Modified, 60, 12, 300));
        d.feed(&ev("a.md", RawFsEventKind::Modified, 90, 12, 300));
        assert_eq!(d.pending_len(), 1, "同一パスは 1 件に合体している");

        // 静穏期間（100ms）経過後に 1 件だけ確定する。
        let out = d.drain_settled(220);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0], FsChange::modified("a.md"));
        assert_eq!(d.pending_len(), 0);
    }

    #[test]
    fn 静穏期間内は確定しない() {
        let mut d = Debouncer::with_debounce(100);
        d.feed(&ev("a.md", RawFsEventKind::Modified, 0, 10, 100));
        d.feed(&ev("a.md", RawFsEventKind::Modified, 50, 10, 100));
        // 最後のイベント(50ms)から 100ms 未満では確定しない。
        assert!(d.drain_settled(120).is_empty());
        // 経過後は確定する。
        assert_eq!(d.drain_settled(160).len(), 1);
    }

    #[test]
    fn メタが安定するまで中途内容で確定しない() {
        // mtime/サイズが変化し続ける間（書き込み継続中）は静穏期間を過ぎても確定しない。
        let mut d = Debouncer::with_debounce(100);
        d.feed(&ev("big.md", RawFsEventKind::Modified, 0, 1, 100));
        d.feed(&ev("big.md", RawFsEventKind::Modified, 10, 2, 200)); // 変化中
                                                                     // 静穏期間は過ぎているがメタが安定していない（prev != last）。
        assert!(
            d.drain_settled(200).is_empty(),
            "メタ未安定なら確定しない（末尾欠損防止）"
        );
        // 同値が 2 連続したら安定とみなし確定する。
        d.feed(&ev("big.md", RawFsEventKind::Modified, 210, 3, 300));
        d.feed(&ev("big.md", RawFsEventKind::Modified, 220, 3, 300));
        let out = d.drain_settled(400);
        assert_eq!(out, vec![FsChange::modified("big.md")]);
    }

    #[test]
    fn メタ取得不能_fs_は時間のみで確定する() {
        // ネットワーク/クラウドで mtime/サイズが取れない場合でも静穏期間後に確定する（縮退維持）。
        let mut d = Debouncer::with_debounce(100);
        d.feed(&ev_no_meta("net.md", RawFsEventKind::Modified, 0));
        assert!(d.drain_settled(50).is_empty());
        assert_eq!(d.drain_settled(150), vec![FsChange::modified("net.md")]);
    }

    #[test]
    fn 削除は安定確認なしで確定する() {
        let mut d = Debouncer::with_debounce(100);
        d.feed(&ev("gone.md", RawFsEventKind::Removed, 0, 0, 0));
        assert_eq!(d.drain_settled(150), vec![FsChange::removed("gone.md")]);
    }

    #[test]
    fn created_のあと_modified_は_created_のまま() {
        let mut d = Debouncer::with_debounce(100);
        d.feed(&ev("new.md", RawFsEventKind::Created, 0, 1, 10));
        d.feed(&ev("new.md", RawFsEventKind::Modified, 10, 2, 20));
        // 安定確認用に同値 2 連続。
        d.feed(&ev("new.md", RawFsEventKind::Modified, 20, 3, 30));
        d.feed(&ev("new.md", RawFsEventKind::Modified, 30, 3, 30));
        let out = d.drain_settled(200);
        assert_eq!(out, vec![FsChange::created("new.md")]);
    }

    #[test]
    fn removed_のあと_created_は_modified_に倒す() {
        // 消えて作り直し＝外部変更（内容変更）として扱う（安全側）。
        let mut d = Debouncer::with_debounce(100);
        d.feed(&ev("x.md", RawFsEventKind::Removed, 0, 0, 0));
        d.feed(&ev("x.md", RawFsEventKind::Created, 10, 1, 50));
        d.feed(&ev("x.md", RawFsEventKind::Modified, 20, 1, 50)); // 安定
        let out = d.drain_settled(200);
        assert_eq!(out, vec![FsChange::modified("x.md")]);
    }

    #[test]
    fn 複数パスは独立に確定する() {
        let mut d = Debouncer::with_debounce(100);
        d.feed(&ev("a.md", RawFsEventKind::Modified, 0, 1, 1));
        d.feed(&ev("a.md", RawFsEventKind::Modified, 10, 1, 1));
        d.feed(&ev("b.md", RawFsEventKind::Created, 90, 2, 2));
        d.feed(&ev("b.md", RawFsEventKind::Created, 95, 2, 2));
        // a は確定、b はまだ静穏期間内。
        let out = d.drain_settled(150);
        assert_eq!(out, vec![FsChange::modified("a.md")]);
        assert_eq!(d.pending_len(), 1);
    }
}
