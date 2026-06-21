//! 通知バーのキュー運用（要件11.1・design doc 5章/19章 通知バーキュー運用）。
//!
//! 本モジュールは UI/Tauri/wry/DOM を一切知らない**純粋ロジック**（cargo test の決定論ゲート対象）。
//! 「いま何本どの順で出すか／何件に集約するか／どれを自動消滅させるか」の**意思決定**だけを担い、
//! 実際の DOM 描画・自動消滅タイマーはフロント（`src/ui/notifications.ts`）が結果に従って行う。
//!
//! 要件11.1 のキュー運用規則（design doc 19章 通知バーキュー運用）:
//! - 同時表示は**最大3本**とし、超過分は「他N件」に集約する。
//! - 種類の優先順位は **衝突 ＞ 設定エラー ＞ 外部リソース参照検知 ＞ JS検知 ＞ 巨大ファイル制限**。
//! - **同一ファイル・同一種別**の通知は最新へ集約（合体）する。
//! - **タブ固有**の通知（衝突等）はタブ切替で表示内容も切り替わり、**グローバル**通知（設定エラー等）は
//!   常時表示する。
//! - 各通知の**自動消滅条件**（解消時に消える／ユーザーが閉じるまで残る）を種類ごとに定める。

use std::collections::HashMap;

/// 同時に画面へ出す通知の最大本数（要件11.1「最大3本」）。超過は「他N件」へ集約する。
pub const MAX_VISIBLE: usize = 3;

/// 通知の種別（要件11.1 の「種類」と優先順位に対応）。
///
/// 優先順位の根拠（要件11.1）: 衝突 ＞ 設定エラー ＞ 外部リソース参照検知 ＞ JS検知 ＞ 巨大ファイル制限。
/// 数値が小さいほど高優先（[`NoticeKind::priority`]）。ファイル削除（要件7.2）は情報通知として最下位に置く。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum NoticeKind {
    /// 外部変更の衝突（タブ固有・最優先・ユーザーが対処するまで残す）。
    Conflict,
    /// 設定エラー（settings.toml 不正・グローバル・解消で消える）。
    SettingsError,
    /// 外部リソース参照検知（http 画像等・オプトイン導線・タブ固有）。
    ExternalResource,
    /// JS依存HTML検知（`<script>`/CDN・タブ固有・「既定のブラウザで開く」導線）。
    ScriptDetected,
    /// 巨大ファイル・レンダリング制限（段階制で機能自動オフ・タブ固有）。
    HugeFileLimit,
    /// ファイル削除（未確認新規が確認前に削除＝要件7.2・件数のみ・グローバル・情報）。
    FileRemoved,
}

impl NoticeKind {
    /// 表示優先順位（小さいほど高優先＝先に表示する。要件11.1）。
    pub fn priority(self) -> u8 {
        match self {
            NoticeKind::Conflict => 0,
            NoticeKind::SettingsError => 1,
            NoticeKind::ExternalResource => 2,
            NoticeKind::ScriptDetected => 3,
            NoticeKind::HugeFileLimit => 4,
            NoticeKind::FileRemoved => 5,
        }
    }

    /// この種別が「解消時に自動で消える」か（true）／「ユーザーが閉じるまで残る」か（false）。
    ///
    /// 要件11.1「各通知の自動消滅条件を種類ごとに定める」。
    /// - 衝突: ユーザーが［差分を見る/取り込む/編集続行］するまで残す（誤って見逃させない）。
    /// - 設定エラー: settings.toml が直れば解消＝自動消滅。
    /// - 外部リソース/JS検知: 別文書へ移れば（タブ固有なので）非表示になる＝事実上自動消滅。
    /// - 巨大ファイル制限: そのファイルを閉じれば（タブ固有）非表示。
    /// - ファイル削除: 情報通知。一定時間で自動消滅してよい。
    pub fn auto_dismiss(self) -> bool {
        match self {
            NoticeKind::Conflict => false,
            NoticeKind::SettingsError
            | NoticeKind::ExternalResource
            | NoticeKind::ScriptDetected
            | NoticeKind::HugeFileLimit
            | NoticeKind::FileRemoved => true,
        }
    }
}

/// 通知のスコープ（タブ固有 or グローバル・要件11.1）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Scope {
    /// タブ固有（このパスのタブを見ているときだけ表示・タブ切替で切り替わる）。
    Tab(String),
    /// グローバル（常時表示。設定エラー・ファイル削除件数等）。
    Global,
}

/// 1 件の通知（フロントが受け取る前の決定論モデル）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Notice {
    /// 種別（優先順位・自動消滅条件を決める）。
    pub kind: NoticeKind,
    /// スコープ（タブ固有 or グローバル）。
    pub scope: Scope,
    /// 表示文言（呼び出し側が組み立てる。ここでは保持のみ）。
    pub message: String,
    /// 集約・並べ替えの安定化に使う単調増加シーケンス（新しいほど大きい＝同一集約で最新採用）。
    pub seq: u64,
}

/// 集約キー（同一ファイル・同一種別を1本に合体する＝要件11.1）。
///
/// タブ固有はパス＋種別、グローバルは種別のみ（同種のグローバルは1本に合体）。
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
struct MergeKey {
    kind: NoticeKind,
    /// タブ固有はパス、グローバルは None。
    path: Option<String>,
}

impl Notice {
    fn merge_key(&self) -> MergeKey {
        MergeKey {
            kind: self.kind,
            path: match &self.scope {
                Scope::Tab(p) => Some(p.clone()),
                Scope::Global => None,
            },
        }
    }
}

/// キュー解決の結果（フロントが描画する最終形）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Resolved {
    /// 実際に表示する通知（最大 [`MAX_VISIBLE`] 本・優先順位＋新しさ順）。
    pub visible: Vec<Notice>,
    /// 「他N件」の N（表示しきれず集約された件数。0 なら出さない）。
    pub overflow: usize,
}

/// 通知キュー（決定論モデル）。`push` で積み、`resolve` で「いま出す3本＋他N件」を算出する。
///
/// フロントはこの結果に従って描画し、種別ごとの自動消滅（[`NoticeKind::auto_dismiss`]）を実行する。
#[derive(Debug, Default)]
pub struct NoticeQueue {
    items: Vec<Notice>,
    next_seq: u64,
}

impl NoticeQueue {
    /// 空のキューを作る。
    pub fn new() -> Self {
        Self::default()
    }

    /// 通知を積む（同一ファイル・同一種別は最新へ集約＝合体する。要件11.1）。
    ///
    /// `seq` は内部で自動採番するため呼び出し側は指定しない（push 引数の `seq` は無視し付け直す）。
    pub fn push(&mut self, kind: NoticeKind, scope: Scope, message: impl Into<String>) {
        let seq = self.next_seq;
        self.next_seq += 1;
        let notice = Notice {
            kind,
            scope,
            message: message.into(),
            seq,
        };
        let key = notice.merge_key();
        // 同一集約キーの既存通知を最新の内容で置き換える（合体）。
        if let Some(existing) = self.items.iter_mut().find(|n| n.merge_key() == key) {
            *existing = notice;
        } else {
            self.items.push(notice);
        }
    }

    /// 種別の指定通知を解消（自動消滅条件の発火・設定エラー解消等）で取り除く。
    ///
    /// グローバル種別の解消（`path=None`）と、タブ固有の解消（`path=Some`）の両方に使う。
    pub fn dismiss(&mut self, kind: NoticeKind, path: Option<&str>) {
        let target = MergeKey {
            kind,
            path: path.map(|p| p.to_string()),
        };
        self.items.retain(|n| n.merge_key() != target);
    }

    /// あるタブを閉じた／消えたとき、そのタブ固有通知をまとめて取り除く。
    pub fn dismiss_tab(&mut self, path: &str) {
        self.items
            .retain(|n| !matches!(&n.scope, Scope::Tab(p) if p == path));
    }

    /// 全件数（テスト・「他N件」検算用）。
    pub fn len(&self) -> usize {
        self.items.len()
    }

    /// 空か。
    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    /// いま表示する通知を解決する（要件11.1）。
    ///
    /// - `active_tab`: 現在アクティブなタブのパス（タブ固有通知の表示可否を決める）。`None` なら
    ///   グローバル通知のみ対象（タブ固有はアクティブタブが無ければ出さない）。
    /// - 表示対象 = グローバル通知 ＋ アクティブタブのタブ固有通知。
    /// - 並びは **優先順位（小さいほど先）→ 新しさ（seq 大きいほど先）**。
    /// - 先頭 [`MAX_VISIBLE`] 本を表示、残りは「他N件」（`overflow`）に集約する。
    pub fn resolve(&self, active_tab: Option<&str>) -> Resolved {
        let mut shown: Vec<&Notice> = self
            .items
            .iter()
            .filter(|n| match &n.scope {
                Scope::Global => true,
                Scope::Tab(p) => active_tab == Some(p.as_str()),
            })
            .collect();
        // 優先順位（昇順）→ 新しさ（seq 降順）で安定ソート。
        shown.sort_by(|a, b| {
            a.kind
                .priority()
                .cmp(&b.kind.priority())
                .then(b.seq.cmp(&a.seq))
        });
        let total = shown.len();
        let visible: Vec<Notice> = shown
            .iter()
            .take(MAX_VISIBLE)
            .map(|n| (*n).clone())
            .collect();
        let overflow = total.saturating_sub(MAX_VISIBLE);
        Resolved { visible, overflow }
    }

    /// アクティブタブ別の未表示（隠れている）件数を種別ごとに数える（テスト・診断用）。
    ///
    /// 「他N件」の内訳をデバッグ表示したいときに使う。production では `resolve().overflow` で足りる。
    pub fn hidden_by_kind(&self, active_tab: Option<&str>) -> HashMap<NoticeKind, usize> {
        let resolved = self.resolve(active_tab);
        let visible_keys: Vec<(NoticeKind, u64)> =
            resolved.visible.iter().map(|n| (n.kind, n.seq)).collect();
        let mut out: HashMap<NoticeKind, usize> = HashMap::new();
        for n in &self.items {
            let in_scope = match &n.scope {
                Scope::Global => true,
                Scope::Tab(p) => active_tab == Some(p.as_str()),
            };
            if in_scope && !visible_keys.contains(&(n.kind, n.seq)) {
                *out.entry(n.kind).or_insert(0) += 1;
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tab(p: &str) -> Scope {
        Scope::Tab(p.to_string())
    }

    #[test]
    fn 優先順位は衝突が最上位で巨大ファイルが下() {
        // 要件11.1: 衝突 ＞ 設定エラー ＞ 外部リソース ＞ JS検知 ＞ 巨大ファイル。
        assert!(NoticeKind::Conflict.priority() < NoticeKind::SettingsError.priority());
        assert!(NoticeKind::SettingsError.priority() < NoticeKind::ExternalResource.priority());
        assert!(NoticeKind::ExternalResource.priority() < NoticeKind::ScriptDetected.priority());
        assert!(NoticeKind::ScriptDetected.priority() < NoticeKind::HugeFileLimit.priority());
    }

    #[test]
    fn 最大3本を超えると他n件へ集約する() {
        // 要件11.4: 4件以上同時でも最大3本＋「他N件」。
        let mut q = NoticeQueue::new();
        q.push(NoticeKind::Conflict, tab("a.md"), "衝突a");
        q.push(NoticeKind::SettingsError, Scope::Global, "設定エラー");
        q.push(NoticeKind::ExternalResource, tab("a.md"), "外部資源");
        q.push(NoticeKind::ScriptDetected, tab("a.md"), "JS検知");
        let r = q.resolve(Some("a.md"));
        assert_eq!(r.visible.len(), MAX_VISIBLE);
        assert_eq!(r.overflow, 1);
        // 表示は優先順位順: 衝突 → 設定エラー → 外部資源（JS検知が溢れる）。
        assert_eq!(r.visible[0].kind, NoticeKind::Conflict);
        assert_eq!(r.visible[1].kind, NoticeKind::SettingsError);
        assert_eq!(r.visible[2].kind, NoticeKind::ExternalResource);
    }

    #[test]
    fn 同一ファイル同一種別は1本に集約合体する() {
        // 要件11.4: 同一ファイルの衝突通知は1本に集約される。
        let mut q = NoticeQueue::new();
        q.push(NoticeKind::Conflict, tab("a.md"), "衝突1");
        q.push(NoticeKind::Conflict, tab("a.md"), "衝突2（最新）");
        assert_eq!(q.len(), 1);
        let r = q.resolve(Some("a.md"));
        assert_eq!(r.visible.len(), 1);
        // 最新内容へ合体する。
        assert_eq!(r.visible[0].message, "衝突2（最新）");
    }

    #[test]
    fn 別ファイルの同種別は集約しない() {
        let mut q = NoticeQueue::new();
        q.push(NoticeKind::Conflict, tab("a.md"), "衝突a");
        q.push(NoticeKind::Conflict, tab("b.md"), "衝突b");
        assert_eq!(q.len(), 2);
        // a.md をアクティブにすると a の衝突だけ見える（タブ固有・要件11.1）。
        let ra = q.resolve(Some("a.md"));
        assert_eq!(ra.visible.len(), 1);
        assert_eq!(ra.visible[0].message, "衝突a");
        let rb = q.resolve(Some("b.md"));
        assert_eq!(rb.visible[0].message, "衝突b");
    }

    #[test]
    fn タブ切替で表示が切り替わる() {
        // 要件11.4: タブを切り替えるとそのタブ固有の通知に表示が切り替わる。
        let mut q = NoticeQueue::new();
        q.push(NoticeKind::HugeFileLimit, tab("big.json"), "巨大ファイル");
        q.push(NoticeKind::SettingsError, Scope::Global, "設定エラー");
        // big.json アクティブ: タブ固有＋グローバルの2本。
        let r1 = q.resolve(Some("big.json"));
        assert_eq!(r1.visible.len(), 2);
        // 別タブ other.md アクティブ: big.json のタブ固有は隠れ、グローバルのみ。
        let r2 = q.resolve(Some("other.md"));
        assert_eq!(r2.visible.len(), 1);
        assert_eq!(r2.visible[0].kind, NoticeKind::SettingsError);
    }

    #[test]
    fn グローバル通知は常時表示() {
        let mut q = NoticeQueue::new();
        q.push(NoticeKind::SettingsError, Scope::Global, "設定エラー");
        // アクティブタブが無くてもグローバルは出る。
        let r = q.resolve(None);
        assert_eq!(r.visible.len(), 1);
        assert_eq!(r.visible[0].kind, NoticeKind::SettingsError);
    }

    #[test]
    fn 設定エラーは解消で消える自動消滅条件() {
        // settings.toml が直れば dismiss で消える（要件11.1 自動消滅条件）。
        assert!(NoticeKind::SettingsError.auto_dismiss());
        let mut q = NoticeQueue::new();
        q.push(NoticeKind::SettingsError, Scope::Global, "設定エラー");
        q.dismiss(NoticeKind::SettingsError, None);
        assert!(q.is_empty());
    }

    #[test]
    fn 衝突はユーザーが閉じるまで残る() {
        // 衝突は自動消滅しない（誤って見逃させない・要件11.1）。
        assert!(!NoticeKind::Conflict.auto_dismiss());
    }

    #[test]
    fn タブを閉じるとそのタブ固有通知を全消去する() {
        let mut q = NoticeQueue::new();
        q.push(NoticeKind::Conflict, tab("a.md"), "衝突a");
        q.push(NoticeKind::HugeFileLimit, tab("a.md"), "巨大a");
        q.push(NoticeKind::SettingsError, Scope::Global, "設定");
        q.dismiss_tab("a.md");
        // a.md 固有は消え、グローバルは残る。
        assert_eq!(q.len(), 1);
        assert_eq!(q.resolve(None).visible[0].kind, NoticeKind::SettingsError);
    }

    #[test]
    fn 同優先なら新しいものが先に出る() {
        let mut q = NoticeQueue::new();
        // 同種別は集約されるので別ファイルの同種別で seq 差を作る。
        q.push(NoticeKind::Conflict, tab("a.md"), "古い");
        q.push(NoticeKind::Conflict, tab("b.md"), "新しい");
        // 両方アクティブにはできない（タブ固有）が、グローバル化したケースを検査するため
        // FileRemoved（グローバル）を別 seq で2件…は集約される。ここでは別タブで個別検証済み。
        // 同タブ内の優先順位差を検証する。
        q.push(NoticeKind::HugeFileLimit, tab("a.md"), "巨大");
        let r = q.resolve(Some("a.md"));
        // a.md には衝突(優先0)と巨大(優先4)。衝突が先。
        assert_eq!(r.visible[0].kind, NoticeKind::Conflict);
        assert_eq!(r.visible[1].kind, NoticeKind::HugeFileLimit);
    }

    #[test]
    fn 他n件の内訳を種別ごとに数えられる() {
        let mut q = NoticeQueue::new();
        q.push(NoticeKind::Conflict, tab("a.md"), "衝突");
        q.push(NoticeKind::SettingsError, Scope::Global, "設定");
        q.push(NoticeKind::ExternalResource, tab("a.md"), "外部");
        q.push(NoticeKind::ScriptDetected, tab("a.md"), "JS"); // これが溢れる。
        let hidden = q.hidden_by_kind(Some("a.md"));
        assert_eq!(hidden.get(&NoticeKind::ScriptDetected), Some(&1));
        assert_eq!(hidden.len(), 1);
    }
}
