//! 容量管理（全体500MB＋90日GC・14日保護・共有object全参照確認後削除・要件9.3・design doc 11章）。
//!
//! 決定論ロジック（cargo test の決定論ゲート対象）。実 FS 削除は呼び出し側が [`GcPlan`] に従って行う。
//!
//! 削除の適用順（要件9.3）:
//! 1. **ファイルごと最新10件 LRU**（[`crate::snapshot::store`] で索引時に適用済み）。
//! 2. **全体容量 GC**（500MB 超過分を古い退避から削除。ベースラインは対象外）。
//! 3. **90日 GC**（最後に開いてから90日経過したスナップショット一式。ベースライン/保護退避は除外）。
//!
//! 保護（削除しない・要件9.3）:
//! - **ベースライン**は容量GC・90日GCのいずれでも削除しない。
//! - **未復元かつ生成から14日以内の退避**（conflict/incoming/rollback/baseline-replace）は容量GC対象外。
//!   保護分だけで上限を超える場合は削除せず呼び出し側が通知バーで復元/破棄を促す。
//! - 共有 object の物理削除は **どのベースライン/退避からも参照されないこと**を確認してから（[`store`] で判定）。

use crate::snapshot::object::StashKind;

/// GC の設定（既定は要件9.3 の数値。settings で調整可だが本層は値を受け取る）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GcConfig {
    /// スナップショット全体の上限（バイト・既定500MB）。
    pub total_limit_bytes: u64,
    /// 未復元退避の保護期間（ミリ秒・既定14日）。
    pub protect_window_ms: u64,
    /// 90日 GC のしきい（ミリ秒・既定90日）。
    pub stale_window_ms: u64,
}

impl Default for GcConfig {
    fn default() -> Self {
        const DAY_MS: u64 = 24 * 60 * 60 * 1000;
        Self {
            total_limit_bytes: 500 * 1024 * 1024,
            protect_window_ms: 14 * DAY_MS,
            stale_window_ms: 90 * DAY_MS,
        }
    }
}

/// GC 対象の退避 object 1 件（容量計算と削除計画の入力）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StashObject {
    /// object ハッシュ。
    pub object_hash: String,
    /// 圧縮後の物理サイズ（バイト）。
    pub size_bytes: u64,
    /// 退避種別。
    pub kind: StashKind,
    /// 生成時刻（ミリ秒・古い順で削除する LRU キー）。
    pub created_at_ms: u64,
    /// 未復元か（true かつ14日以内なら容量GC保護＝要件9.3）。
    pub unrestored: bool,
}

/// GC の削除計画（呼び出し側が実 FS から削除する object ハッシュ列）。
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct GcPlan {
    /// 削除する退避 object（古い順）。
    pub delete_objects: Vec<String>,
    /// 上限超過だが保護で削除できなかった超過バイト（>0 なら呼び出し側が復元/破棄を促す＝要件9.3）。
    pub protected_overflow_bytes: u64,
}

/// 容量GCの計画を立てる（要件9.3）。
///
/// - `stash_objects`: 退避 object の一覧（ベースライン object は渡さない＝削除対象外）。
/// - `baseline_bytes`: ベースライン object の合計サイズ（上限計算に含めるが削除はしない）。
/// - `now_ms`: 現在時刻（14日保護の判定）。
///
/// 手順:
/// 1. 合計（ベースライン＋退避）が上限以下なら削除なし。
/// 2. 超過分を **削除可能（保護対象外）な退避**から **古い順** に削除し、上限以下に収める。
/// 3. 保護退避（未復元かつ14日以内）だけで上限を超える場合は削除せず、超過バイトを返す
///    （呼び出し側が通知バーで復元/破棄を促す＝削除しない）。
pub fn plan_gc(
    config: &GcConfig,
    stash_objects: &[StashObject],
    baseline_bytes: u64,
    now_ms: u64,
) -> GcPlan {
    let stash_total: u64 = stash_objects.iter().map(|o| o.size_bytes).sum();
    let total = baseline_bytes.saturating_add(stash_total);
    if total <= config.total_limit_bytes {
        return GcPlan::default();
    }

    let mut over = total - config.total_limit_bytes;

    // 削除可能（保護対象外）な退避を古い順に並べる。
    let mut deletable: Vec<&StashObject> = stash_objects
        .iter()
        .filter(|o| !is_protected(o, config, now_ms))
        .collect();
    deletable.sort_by_key(|o| o.created_at_ms);

    let mut plan = GcPlan::default();
    for obj in deletable {
        if over == 0 {
            break;
        }
        plan.delete_objects.push(obj.object_hash.clone());
        over = over.saturating_sub(obj.size_bytes);
    }

    // 保護退避だけで上限を超えていれば削除し切れない＝超過バイトを返す（削除しない・要件9.3）。
    plan.protected_overflow_bytes = over;
    plan
}

/// 退避が容量GCの保護対象か（未復元かつ生成から14日以内＝要件9.3）。
fn is_protected(obj: &StashObject, config: &GcConfig, now_ms: u64) -> bool {
    obj.unrestored && now_ms.saturating_sub(obj.created_at_ms) <= config.protect_window_ms
}

/// フォルダを最後に開いてから90日経過で一式削除対象か（ベースライン/保護退避は呼び出し側が除外）。
///
/// 要件9.3「最後に開いてから90日経過したスナップショット一式は自動削除（ベースライン・保護退避は除外）」。
/// 本関数はワークスペース単位の判定のみ行い、除外は呼び出し側が [`is_protected`] で適用する。
pub fn is_stale_workspace(config: &GcConfig, last_opened_at_ms: u64, now_ms: u64) -> bool {
    now_ms.saturating_sub(last_opened_at_ms) > config.stale_window_ms
}

#[cfg(test)]
mod tests {
    use super::*;

    fn obj(hash: &str, size: u64, created: u64, unrestored: bool) -> StashObject {
        StashObject {
            object_hash: hash.into(),
            size_bytes: size,
            kind: StashKind::Conflict,
            created_at_ms: created,
            unrestored,
        }
    }

    fn small_config() -> GcConfig {
        GcConfig {
            total_limit_bytes: 100,
            protect_window_ms: 1000,
            stale_window_ms: 5000,
        }
    }

    #[test]
    fn 上限以下なら削除なし() {
        let cfg = small_config();
        let objs = [obj("a", 30, 1, false), obj("b", 30, 2, false)];
        let plan = plan_gc(&cfg, &objs, 20, 10_000);
        assert!(plan.delete_objects.is_empty());
        assert_eq!(plan.protected_overflow_bytes, 0);
    }

    #[test]
    fn 超過分を古い退避から削除して上限に収める() {
        let cfg = small_config(); // 上限100
                                  // ベースライン20＋退避(40+40+40=120)=140 → 40 超過。
        let objs = [
            obj("old", 40, 1, false),
            obj("mid", 40, 2, false),
            obj("new", 40, 3, false),
        ];
        let plan = plan_gc(&cfg, &objs, 20, 10_000);
        // 最古 old を1件消すと 100。
        assert_eq!(plan.delete_objects, vec!["old".to_string()]);
        assert_eq!(plan.protected_overflow_bytes, 0);
    }

    #[test]
    fn 未復元かつ14日以内の退避は容量gc保護() {
        let cfg = small_config(); // protect_window=1000
                                  // 全て未復元かつ now=500 で 1000ms 以内＝全保護。
        let objs = [obj("a", 80, 100, true), obj("b", 80, 200, true)];
        let plan = plan_gc(&cfg, &objs, 0, 500);
        // 削除できない（保護）→ 超過バイトを返す（160-100=60）。
        assert!(plan.delete_objects.is_empty());
        assert_eq!(plan.protected_overflow_bytes, 60);
    }

    #[test]
    fn 復元済みは保護されず削除候補() {
        let cfg = small_config();
        // a は復元済み（保護外）・b は未復元保護。a を消して上限に収める。
        let objs = [obj("a", 80, 100, false), obj("b", 80, 200, true)];
        let plan = plan_gc(&cfg, &objs, 0, 500);
        assert_eq!(plan.delete_objects, vec!["a".to_string()]);
        assert_eq!(plan.protected_overflow_bytes, 0);
    }

    #[test]
    fn 保護退避と削除可能退避が混在し削除しても上限に収まらない() {
        // 削除可能退避を全部消しても保護退避が大きく上限超過が残るケース（#38）。
        let cfg = small_config(); // 上限100・protect_window=1000
                                  // a=削除可能(復元済み40)・b,c=保護(未復元かつ14日以内 各60)
                                  // 合計 40+60+60=160 → 60 超過。
        let objs = [
            obj("a", 40, 100, false), // 復元済み＝削除可能。
            obj("b", 60, 200, true),  // 未復元・now-created=300<=1000 で保護。
            obj("c", 60, 300, true),  // 未復元・保護。
        ];
        let plan = plan_gc(&cfg, &objs, 0, 500);
        // 削除は実行する（削除可能な a は消す）。
        assert_eq!(plan.delete_objects, vec!["a".to_string()]);
        // a を消しても 120 で上限100 を 20 超過＝保護退避分の超過バイトを返す（削除しない・要件9.3）。
        assert_eq!(plan.protected_overflow_bytes, 20);
    }

    #[test]
    fn 保護期間を過ぎた未復元は削除候補() {
        let cfg = small_config(); // protect_window=1000
                                  // now=5000・created=100 → 経過4900>1000 で保護切れ。
        let objs = [obj("a", 80, 100, true), obj("b", 80, 200, true)];
        let plan = plan_gc(&cfg, &objs, 0, 5000);
        assert_eq!(plan.delete_objects, vec!["a".to_string()]);
    }

    #[test]
    fn _90日経過ワークスペースは_stale() {
        let cfg = small_config(); // stale_window=5000
        assert!(!is_stale_workspace(&cfg, 0, 5000)); // ちょうどは stale でない（> 判定）。
        assert!(is_stale_workspace(&cfg, 0, 5001));
    }
}
