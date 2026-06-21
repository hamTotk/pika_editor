//! オーバーフロー再同期（要件7.1/7.4・design doc 166行）。
//!
//! `ReadDirectoryChangesW` のカーネルバッファが溢れた（`ERROR_NOTIFY_ENUM_DIR`）場合、
//! 個々のイベントは失われている。そこで該当監視ルートを**全再列挙**し、
//! 各ファイルの mtime/サイズ→（必要時）ハッシュをベースラインと比較して
//! 未読・ベースラインを**取りこぼしなく**再同期する。
//!
//! 同じ再同期処理を **ポーリングフォールバック（監視不能 FS）** と **F5（オンデマンド）** が
//! 共有する（要件7.1）。監視スレッドが FS から fingerprint を採取し、本層は
//! ベースライン台帳との比較だけを行う純粋ロジックに徹する（決定論を保つ＝cargo test 可能）。

use crate::watcher::event::FsChange;
use std::collections::BTreeMap;

/// 再列挙で採取した 1 ファイルの軽量指紋（mtime/サイズ→不一致のみハッシュ）。
///
/// `content_hash` はプレスクリーン（mtime/サイズ）で疑わしいものだけ算出して詰める。
/// 一致確実なものは `None` でよい（ハッシュ計算を省く＝固まらない・軽い）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FileFingerprint {
    /// 更新時刻（ミリ秒）。
    pub mtime_ms: u64,
    /// サイズ（バイト）。
    pub size: u64,
    /// LF 正規化後の内容ハッシュ（mtime/サイズが疑わしい時のみ算出して詰める）。
    pub content_hash: Option<String>,
}

/// ベースライン台帳の 1 エントリ（前回確認時点の指紋）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BaselineEntry {
    /// 前回確認時点の mtime（ミリ秒）。
    pub mtime_ms: u64,
    /// 前回確認時点のサイズ（バイト）。
    pub size: u64,
    /// 前回確認時点の内容ハッシュ。
    pub content_hash: String,
}

/// 再同期の結果（取りこぼしなく未読/削除/新規を算定）。
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ResyncOutcome {
    /// 内容変更で未読化するパス（昇順）。
    pub modified: Vec<String>,
    /// 新規作成で未読化するパス（昇順）。
    pub created: Vec<String>,
    /// 消失（削除扱い）するパス（昇順）。
    pub removed: Vec<String>,
}

impl ResyncOutcome {
    /// フロントへ送る合成結果のリストへ写す（modified→created→removed の順）。
    pub fn into_changes(self) -> Vec<FsChange> {
        let mut out =
            Vec::with_capacity(self.modified.len() + self.created.len() + self.removed.len());
        out.extend(self.modified.into_iter().map(FsChange::modified));
        out.extend(self.created.into_iter().map(FsChange::created));
        out.extend(self.removed.into_iter().map(FsChange::removed));
        out
    }

    /// 検知した変更件数の合計（テスト/診断用）。
    pub fn total(&self) -> usize {
        self.modified.len() + self.created.len() + self.removed.len()
    }
}

/// 再列挙結果 `current` をベースライン台帳 `baseline` と突き合わせて再同期する。
///
/// 判定（design doc 166行・要件7.4）:
/// - ベースラインに無く current にある = **新規**。
/// - ベースラインにあり current に無い = **削除**。
/// - 両方にあり、mtime/サイズが一致 = 未変更（プレスクリーンで除外。LF 差は別途ハッシュで吸収）。
/// - 両方にあり、mtime/サイズが不一致:
///   - current 側に `content_hash` があれば、ベースラインのハッシュと比較し**一致なら未変更**
///     （mtime だけ動いたが内容同一＝LF 正規化照合と整合）、不一致なら **変更**。
///   - `content_hash` が無ければ（プレスクリーンで疑い無しと判断された等）安全側で **変更扱い**。
///
/// `BTreeMap` を使い結果が**昇順で決定論的**になるようにする（テストで取りこぼし無しを観測）。
pub fn resync_against_baseline(
    baseline: &BTreeMap<String, BaselineEntry>,
    current: &BTreeMap<String, FileFingerprint>,
) -> ResyncOutcome {
    let mut outcome = ResyncOutcome::default();

    for (path, cur) in current {
        match baseline.get(path) {
            None => outcome.created.push(path.clone()),
            Some(base) => {
                if cur.mtime_ms == base.mtime_ms && cur.size == base.size {
                    // プレスクリーン一致＝未変更。
                    continue;
                }
                // mtime/サイズが動いた。ハッシュがあれば内容で最終判定（LF 差を吸収）。
                match &cur.content_hash {
                    Some(h) if *h == base.content_hash => {
                        // 内容同一（改行のみの差など）＝未変更。未読化しない。
                    }
                    _ => outcome.modified.push(path.clone()),
                }
            }
        }
    }

    for path in baseline.keys() {
        if !current.contains_key(path) {
            outcome.removed.push(path.clone());
        }
    }

    outcome
}

#[cfg(test)]
mod tests {
    use super::*;

    fn base(mtime: u64, size: u64, hash: &str) -> BaselineEntry {
        BaselineEntry {
            mtime_ms: mtime,
            size,
            content_hash: hash.into(),
        }
    }
    fn cur(mtime: u64, size: u64, hash: Option<&str>) -> FileFingerprint {
        FileFingerprint {
            mtime_ms: mtime,
            size,
            content_hash: hash.map(|s| s.to_string()),
        }
    }

    #[test]
    fn 新規と削除と変更を取りこぼさず算定する() {
        let mut baseline = BTreeMap::new();
        baseline.insert("keep.md".to_string(), base(100, 10, "h-keep"));
        baseline.insert("gone.md".to_string(), base(100, 20, "h-gone"));
        baseline.insert("changed.md".to_string(), base(100, 30, "h-old"));

        let mut current = BTreeMap::new();
        current.insert("keep.md".to_string(), cur(100, 10, None)); // 未変更
        current.insert("changed.md".to_string(), cur(200, 35, Some("h-new"))); // 変更
        current.insert("new.md".to_string(), cur(300, 5, None)); // 新規
                                                                 // gone.md は current に無い＝削除

        let r = resync_against_baseline(&baseline, &current);
        assert_eq!(r.modified, vec!["changed.md".to_string()]);
        assert_eq!(r.created, vec!["new.md".to_string()]);
        assert_eq!(r.removed, vec!["gone.md".to_string()]);
    }

    #[test]
    fn mtime_だけ動いて内容同一なら未変更() {
        // LF 正規化照合と整合: mtime が動いてもハッシュ一致なら未読化しない。
        let mut baseline = BTreeMap::new();
        baseline.insert("a.md".to_string(), base(100, 50, "same"));
        let mut current = BTreeMap::new();
        current.insert("a.md".to_string(), cur(999, 50, Some("same")));
        let r = resync_against_baseline(&baseline, &current);
        assert_eq!(r.total(), 0);
    }

    #[test]
    fn サイズ不一致でハッシュ無しは安全側で変更扱い() {
        let mut baseline = BTreeMap::new();
        baseline.insert("a.md".to_string(), base(100, 50, "old"));
        let mut current = BTreeMap::new();
        // mtime/サイズが動いたがハッシュ未算出 → 安全側で変更扱い（取りこぼさない）。
        current.insert("a.md".to_string(), cur(200, 60, None));
        let r = resync_against_baseline(&baseline, &current);
        assert_eq!(r.modified, vec!["a.md".to_string()]);
    }

    #[test]
    fn 百件同時新規を全件取りこぼさない() {
        // 要件7.4「100件同時変更で取りこぼさない」の決定論側。
        let baseline = BTreeMap::new();
        let mut current = BTreeMap::new();
        for i in 0..100 {
            current.insert(format!("f{i:03}.md"), cur(i as u64, i as u64, None));
        }
        let r = resync_against_baseline(&baseline, &current);
        assert_eq!(r.created.len(), 100);
        // 昇順で決定論的。
        assert_eq!(r.created.first().unwrap(), "f000.md");
        assert_eq!(r.created.last().unwrap(), "f099.md");
    }

    #[test]
    fn 合成結果への写しは_modified_created_removed_の順() {
        let mut baseline = BTreeMap::new();
        baseline.insert("gone.md".to_string(), base(1, 1, "g"));
        baseline.insert("chg.md".to_string(), base(1, 1, "old"));
        let mut current = BTreeMap::new();
        current.insert("chg.md".to_string(), cur(2, 2, Some("new")));
        current.insert("new.md".to_string(), cur(3, 3, None));
        let changes = resync_against_baseline(&baseline, &current).into_changes();
        assert_eq!(
            changes,
            vec![
                FsChange::modified("chg.md"),
                FsChange::created("new.md"),
                FsChange::removed("gone.md"),
            ]
        );
    }
}
