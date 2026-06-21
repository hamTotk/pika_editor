//! ベースライン/退避の索引・参照計数・index 破損復元（要件9.1/9.2・design doc 11章）。
//!
//! 決定論モデル（cargo test の決定論ゲート対象）。永続化（FS 書込・DACL・zstd）は呼び出し側。
//! 本層は「索引（どのファイルが・どの object を・どう参照しているか）」と
//! 「参照計数・容量管理の素・index 破損からの退避一覧再生成」を担う。
//!
//! 設計の核（最上位原則1「データを失わない」）:
//! - 退避は **ファイルごとに最新10件 LRU**（要件9.2）。
//! - object は **content-addressed で重複排除・共有**し、物理削除は **全参照不在を確認後**のみ（要件9.3）。
//! - object に **自己記述メタ**を併記し、index 破損時は object 走査から退避一覧を再生成（最後の砦に到達可能）。

use crate::snapshot::object::{ObjectMeta, StashKind};
use std::collections::BTreeMap;

/// ファイルごとの退避上限（最新10件 LRU＝要件9.2）。
pub const MAX_STASH_PER_FILE: usize = 10;

/// スナップショット層のエラー（最上位原則: 退避結合の失敗を握り潰さない＝呼び出し側へ Result で返す）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SnapshotError {
    /// 退避（object 保存）に失敗。ベースラインを進めてはならない（未読維持＝データを失わない）。
    StashFailed(String),
    /// 内容を保存しない方針（機密/10MB以上/画像）への内容ベースライン化を要求された。
    ContentNotStorable(String),
}

impl std::fmt::Display for SnapshotError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SnapshotError::StashFailed(m) => write!(f, "退避に失敗（ベースライン未更新）: {m}"),
            SnapshotError::ContentNotStorable(m) => write!(f, "内容を保存できない対象: {m}"),
        }
    }
}

impl std::error::Error for SnapshotError {}

/// ベースライン参照（ファイルごとに常に1件＝要件9.2）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BaselineRef {
    /// LF 正規化後の内容ハッシュ（未読判定はこれで行う。内容保存有無に関わらず持つ）。
    pub content_hash: String,
    /// 内容 object のハッシュ（＝アドレス）。ハッシュのみ記録（機密/10MB以上/画像）では `None`。
    pub object_hash: Option<String>,
}

impl BaselineRef {
    /// 差分・巻き戻しが可能か（内容 object を持つか）。
    pub fn has_content(&self) -> bool {
        self.object_hash.is_some()
    }
}

/// 退避エントリ（conflict/incoming/rollback/baseline-replace の1件）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StashEntry {
    /// 退避種別。
    pub kind: StashKind,
    /// 退避内容 object のハッシュ（自己記述メタ経由で復元できる）。
    pub object_hash: String,
    /// 退避時刻（ミリ秒・LRU の順序キー）。
    pub created_at_ms: u64,
    /// 未復元か（true の間は 14日保護・容量GCの保護対象＝要件9.3）。
    pub unrestored: bool,
}

/// 退避操作の結果（退避が成立したエントリと、LRU で押し出された object を返す）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StashResult {
    /// 追加された退避エントリの object ハッシュ。
    pub stashed_object: String,
    /// LRU 超過で索引から外れた退避 object（呼び出し側は GC 候補として扱う）。
    pub evicted_objects: Vec<String>,
}

/// スナップショット索引の決定論モデル。
///
/// 内容実体（object）は本モデルでは「ハッシュ集合＋メタ」で表す（実バイトは呼び出し側が持つ）。
/// 物理削除可否（参照計数）・LRU・退避一覧再生成を本層で決める。
#[derive(Debug, Default)]
pub struct SnapshotStore {
    /// index 世代（退避メタの整合確認・破損復元に使う。退避/ベースライン更新で前進）。
    generation: u64,
    /// relPath → ベースライン参照（常に1件）。
    baselines: BTreeMap<String, BaselineRef>,
    /// relPath → 退避リスト（新しい順ではなく登録順で持ち、LRU は created_at で判断）。
    stashes: BTreeMap<String, Vec<StashEntry>>,
    /// object ハッシュ → 自己記述メタ（index 破損復元の素・object 台帳の写し）。
    object_meta: BTreeMap<String, ObjectMeta>,
}

impl SnapshotStore {
    /// 空の索引を作る。
    pub fn new() -> Self {
        Self::default()
    }

    /// 現在の index 世代。
    pub fn generation(&self) -> u64 {
        self.generation
    }

    /// ファイルのベースラインを引く。
    pub fn baseline(&self, rel_path: &str) -> Option<&BaselineRef> {
        self.baselines.get(rel_path)
    }

    /// ファイルの退避リスト（登録順）を引く。
    pub fn stashes(&self, rel_path: &str) -> &[StashEntry] {
        self.stashes
            .get(rel_path)
            .map(|v| v.as_slice())
            .unwrap_or(&[])
    }

    /// ベースラインを内容付き（差分・巻き戻し可能）で設定/更新する（要件9.2）。
    ///
    /// `object_hash` は内容 object のハッシュ。`content_hash` は未読判定用の LF 正規化ハッシュ
    /// （内容保存時は両者一致が通常だが、呼び出し側の区別を尊重して両方受ける）。
    pub fn set_baseline_with_content(
        &mut self,
        rel_path: impl Into<String>,
        content_hash: impl Into<String>,
        object_hash: impl Into<String>,
    ) {
        self.generation += 1;
        let rel_path = rel_path.into();
        self.baselines.insert(
            rel_path,
            BaselineRef {
                content_hash: content_hash.into(),
                object_hash: Some(object_hash.into()),
            },
        );
    }

    /// ベースラインをハッシュのみ（機密/10MB以上/画像）で設定/更新する（要件9.1/9.2）。
    pub fn set_baseline_hash_only(
        &mut self,
        rel_path: impl Into<String>,
        content_hash: impl Into<String>,
    ) {
        self.generation += 1;
        self.baselines.insert(
            rel_path.into(),
            BaselineRef {
                content_hash: content_hash.into(),
                object_hash: None,
            },
        );
    }

    /// 退避を追加する（要件9.2 LRU＝ファイルごと最新10件）。
    ///
    /// object メタを併記し（index 破損復元の素）、ファイルごと最新10件を超えたら
    /// **未復元のものを優先保護**しつつ最古から押し出す（押し出された object は GC 候補）。
    /// baseline-replace は呼び出し側が別バッチで管理するため、ここでは通常退避と同じ枠で扱わず
    /// `count_in_lru=false` を指定できる（要件9.2「10件枠とは別」）。
    pub fn add_stash(
        &mut self,
        rel_path: impl Into<String>,
        kind: StashKind,
        object_hash: impl Into<String>,
        created_at_ms: u64,
        count_in_lru: bool,
    ) -> StashResult {
        self.generation += 1;
        let rel_path = rel_path.into();
        let object_hash = object_hash.into();

        self.object_meta.insert(
            object_hash.clone(),
            ObjectMeta {
                rel_path: rel_path.clone(),
                kind,
                created_at_ms,
                index_generation: self.generation,
            },
        );

        let list = self.stashes.entry(rel_path).or_default();
        list.push(StashEntry {
            kind,
            object_hash: object_hash.clone(),
            created_at_ms,
            unrestored: true,
        });

        let mut evicted = Vec::new();
        if count_in_lru {
            evicted = Self::evict_lru(list);
        }
        // 押し出された object は索引から消えるが、物理削除は全参照確認後（呼び出し側が gc で判断）。
        for ev in &evicted {
            // 索引から外れた object のメタも落とす（再生成は残存 object のメタからのみ行う）。
            self.object_meta.remove(ev);
        }

        StashResult {
            stashed_object: object_hash,
            evicted_objects: evicted,
        }
    }

    /// ファイルごと最新10件 LRU を適用し、押し出した object ハッシュを返す。
    ///
    /// 保持優先順位: **未復元 ＞ 復元済み**、同条件なら **新しい ＞ 古い**（要件9.2/9.3）。
    /// 未復元の退避は LRU では消さず（14日保護は容量GC側＝要件9.3）、復元済みの最古から押し出す。
    /// それでも上限超過（未復元だけで10件超）なら最古の未復元から押し出す（索引上限は守る）。
    fn evict_lru(list: &mut Vec<StashEntry>) -> Vec<String> {
        if list.len() <= MAX_STASH_PER_FILE {
            return Vec::new();
        }
        // 押し出し順（先頭が最初に消える）: 復元済みを古い順、その後に未復元を古い順。
        let mut order: Vec<usize> = (0..list.len()).collect();
        order.sort_by(|&a, &b| {
            let ea = &list[a];
            let eb = &list[b];
            // 復元済み(false が unrestored)が先に消える → unrestored 昇順（false<true）。
            ea.unrestored
                .cmp(&eb.unrestored)
                .then(ea.created_at_ms.cmp(&eb.created_at_ms))
        });
        let remove_count = list.len() - MAX_STASH_PER_FILE;
        let mut to_remove: Vec<usize> = order.into_iter().take(remove_count).collect();
        to_remove.sort_unstable();
        let mut evicted = Vec::new();
        // 後ろから抜くと添字がずれない。
        for &idx in to_remove.iter().rev() {
            evicted.push(list.remove(idx).object_hash);
        }
        evicted
    }

    /// 退避を復元済みにマークする（14日保護を外す＝容量GCで落とせるようにする）。
    pub fn mark_restored(&mut self, rel_path: &str, object_hash: &str) {
        if let Some(list) = self.stashes.get_mut(rel_path) {
            for e in list.iter_mut() {
                if e.object_hash == object_hash {
                    e.unrestored = false;
                }
            }
        }
    }

    /// object が索引から参照されているか（ベースライン baselineHash か退避 stash.hash＝要件9.3）。
    ///
    /// 物理削除の可否判定に使う。**全参照不在を確認してから**呼び出し側が object を物理削除する。
    pub fn is_object_referenced(&self, object_hash: &str) -> bool {
        if self
            .baselines
            .values()
            .any(|b| b.object_hash.as_deref() == Some(object_hash))
        {
            return true;
        }
        self.stashes
            .values()
            .flatten()
            .any(|s| s.object_hash == object_hash)
    }

    /// 索引が参照する全 object ハッシュ集合（GC の生存集合＝要件9.3）。
    pub fn live_objects(&self) -> std::collections::BTreeSet<String> {
        let mut set = std::collections::BTreeSet::new();
        for b in self.baselines.values() {
            if let Some(h) = &b.object_hash {
                set.insert(h.clone());
            }
        }
        for s in self.stashes.values().flatten() {
            set.insert(s.object_hash.clone());
        }
        set
    }

    /// index 破損時に object 群の自己記述メタから「復元待ちの退避一覧」を再生成する（要件9.1・最上位原則1）。
    ///
    /// `present_objects` は実在する（走査で見つかった）object ハッシュ集合。
    /// 索引（baselines/stashes）を捨て、メタが残っている object から退避エントリを復元する
    /// （退避＝最後の砦に到達不能にしない）。ベースラインは内容照合で別途取り直す前提のため復元しない。
    /// 戻り値は relPath → 復元した退避リスト（時刻昇順）。
    pub fn recover_stashes_from_meta(
        &self,
        present_objects: &std::collections::BTreeSet<String>,
    ) -> BTreeMap<String, Vec<StashEntry>> {
        let mut out: BTreeMap<String, Vec<StashEntry>> = BTreeMap::new();
        for (hash, meta) in &self.object_meta {
            if !present_objects.contains(hash) {
                continue; // object 実体が無いメタは復元できない。
            }
            out.entry(meta.rel_path.clone())
                .or_default()
                .push(StashEntry {
                    kind: meta.kind,
                    object_hash: hash.clone(),
                    created_at_ms: meta.created_at_ms,
                    unrestored: true, // 復元待ちとして提示（最後の砦）。
                });
        }
        for list in out.values_mut() {
            list.sort_by_key(|e| e.created_at_ms);
        }
        out
    }

    /// （テスト/復元用）破損復元で得た退避一覧を索引へ取り込む。
    pub fn install_recovered_stashes(&mut self, recovered: BTreeMap<String, Vec<StashEntry>>) {
        self.stashes = recovered;
    }

    /// object メタを引く（破損復元の検証/診断用）。
    pub fn object_meta(&self, object_hash: &str) -> Option<&ObjectMeta> {
        self.object_meta.get(object_hash)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeSet;

    fn store_with_baseline() -> SnapshotStore {
        let mut s = SnapshotStore::new();
        s.set_baseline_with_content("a.md", "h-a", "obj-a");
        s
    }

    #[test]
    fn ベースラインは常に1件で上書き更新() {
        let mut s = store_with_baseline();
        assert_eq!(
            s.baseline("a.md").unwrap().object_hash.as_deref(),
            Some("obj-a")
        );
        s.set_baseline_with_content("a.md", "h-a2", "obj-a2");
        // 1件のまま新しい内容へ更新（要件9.2 ファイルごとに常に1つ）。
        assert_eq!(
            s.baseline("a.md").unwrap().object_hash.as_deref(),
            Some("obj-a2")
        );
    }

    #[test]
    fn ハッシュのみベースラインは差分巻き戻し非対象() {
        let mut s = SnapshotStore::new();
        s.set_baseline_hash_only("secret.env", "h-x");
        let b = s.baseline("secret.env").unwrap();
        assert!(
            !b.has_content(),
            "ハッシュのみは内容 object を持たない＝差分/巻き戻し非対象"
        );
        assert_eq!(b.content_hash, "h-x");
    }

    #[test]
    fn 退避はファイルごと最新10件_lru() {
        let mut s = SnapshotStore::new();
        // 復元済みにしておくと LRU で最古から押し出される（未復元保護を外す）。
        for i in 0..12u64 {
            let r = s.add_stash("a.md", StashKind::Conflict, format!("obj-{i}"), i, true);
            s.mark_restored("a.md", &format!("obj-{i}"));
            if i < 10 {
                assert!(r.evicted_objects.is_empty());
            }
        }
        // 12 件入れたが 10 件に収まる。
        assert_eq!(s.stashes("a.md").len(), MAX_STASH_PER_FILE);
        // 最古 obj-0, obj-1 が押し出されている。
        assert!(s.stashes("a.md").iter().all(|e| e.object_hash != "obj-0"));
        assert!(s.stashes("a.md").iter().all(|e| e.object_hash != "obj-1"));
    }

    #[test]
    fn baseline_replace_は10件枠と別で押し出さない() {
        let mut s = SnapshotStore::new();
        // 通常退避で 10 件埋めても、baseline-replace は count_in_lru=false で別枠。
        for i in 0..10u64 {
            s.add_stash("a.md", StashKind::Conflict, format!("c-{i}"), i, true);
        }
        let r = s.add_stash("a.md", StashKind::BaselineReplace, "br-0", 100, false);
        assert!(
            r.evicted_objects.is_empty(),
            "baseline-replace は LRU 枠を消費しない"
        );
        assert_eq!(s.stashes("a.md").len(), 11);
    }

    #[test]
    fn 共有_object_は全参照不在を確認後に物理削除可能() {
        let mut s = SnapshotStore::new();
        // ベースラインと退避が同じ object を共有する状況。
        s.set_baseline_with_content("a.md", "shared", "shared");
        s.add_stash("b.md", StashKind::Rollback, "shared", 1, true);
        // ベースラインが参照中＝削除不可。
        assert!(s.is_object_referenced("shared"));
        // ベースラインを別 object へ更新しても、退避がまだ参照中。
        s.set_baseline_with_content("a.md", "other", "other");
        assert!(s.is_object_referenced("shared"), "退避がまだ shared を参照");
        // 退避側を別 object に置換するまで shared は生存集合に残る。
        assert!(s.live_objects().contains("shared"));
    }

    #[test]
    fn index_破損時に_object_メタから退避一覧を再生成できる() {
        // 退避を積んでから索引（baselines/stashes）を捨て、メタから復元する（最上位原則1）。
        let mut s = SnapshotStore::new();
        s.add_stash("a.md", StashKind::Conflict, "obj-1", 100, true);
        s.add_stash("a.md", StashKind::Incoming, "obj-2", 200, true);
        s.add_stash("b.md", StashKind::Rollback, "obj-3", 300, true);

        // 実在 object（走査で見つかった集合）。
        let present: BTreeSet<String> = ["obj-1", "obj-2", "obj-3"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let recovered = s.recover_stashes_from_meta(&present);

        assert_eq!(recovered.get("a.md").unwrap().len(), 2);
        assert_eq!(recovered.get("b.md").unwrap().len(), 1);
        // 時刻昇順で並ぶ。
        assert_eq!(recovered.get("a.md").unwrap()[0].object_hash, "obj-1");
        // 全て復元待ち（最後の砦として提示）。
        assert!(recovered.values().flatten().all(|e| e.unrestored));
    }

    #[test]
    fn 実体が欠けた_object_メタは復元しない() {
        let mut s = SnapshotStore::new();
        s.add_stash("a.md", StashKind::Conflict, "obj-present", 1, true);
        s.add_stash("a.md", StashKind::Incoming, "obj-missing", 2, true);
        // obj-missing は走査で見つからなかった（実体が無い）。
        let present: BTreeSet<String> = ["obj-present"].iter().map(|s| s.to_string()).collect();
        let recovered = s.recover_stashes_from_meta(&present);
        let list = recovered.get("a.md").unwrap();
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].object_hash, "obj-present");
    }

    #[test]
    fn 未復元退避は_lru_で復元済みより後に押し出す() {
        let mut s = SnapshotStore::new();
        // 11 件目で 1 件押し出し。未復元 5 件・復元済み 6 件にしておくと復元済みの最古が消える。
        for i in 0..11u64 {
            s.add_stash("a.md", StashKind::Conflict, format!("o-{i}"), i, true);
            if i >= 5 {
                s.mark_restored("a.md", &format!("o-{i}")); // o-5..o-10 を復元済みに。
            }
        }
        // 押し出されるのは復元済みの最古 o-5（未復元 o-0..o-4 は保護）。
        assert!(s.stashes("a.md").iter().all(|e| e.object_hash != "o-5"));
        assert!(s.stashes("a.md").iter().any(|e| e.object_hash == "o-0"));
    }
}
