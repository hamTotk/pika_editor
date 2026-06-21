//! 変更行ペアの行内差分（語/grapheme フォールバック・要件8.2・design doc 7章）。
//!
//! 行 LCS（[`crate::diff::line`]）が「置換」と判定した行ペアに対し、**変わった部分だけ**を
//! ハイライトするためのセグメント列を算出する。
//!
//! 自前なのは **「語境界が取れる行か否かの判定（語単位 / grapheme 単位）」のみ**で、
//! LCS 自体は [`similar`] に委ねる（design doc 7章）。
//! 空白区切りの語境界が取れない行（日本語等）は grapheme 単位へフォールバックする（要件8.2）。

use similar::{capture_diff_slices, Algorithm, ChangeTag};
use unicode_segmentation::UnicodeSegmentation;

/// 行内差分の粒度（観測可能にしてテストで語/grapheme フォールバックを検証する）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Granularity {
    /// 空白区切りの語単位（英文など語境界が取れる行）。
    Word,
    /// grapheme 単位（日本語など空白区切りの語境界が取れない行）。
    Grapheme,
}

/// 行内の 1 セグメント（フロントが下線/太字でハイライトする最小単位）。
///
/// `changed=false` は変更行のうち両側で共通の部分（地の文）、`true` は変わった部分。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Segment {
    /// 変わった部分か（フロントが色だけに依存せず下線/太字で表す＝要件8.2/11.5）。
    pub changed: bool,
    /// セグメント本文。
    pub text: String,
}

impl Segment {
    fn equal(text: impl Into<String>) -> Self {
        Self {
            changed: false,
            text: text.into(),
        }
    }
    fn changed(text: impl Into<String>) -> Self {
        Self {
            changed: true,
            text: text.into(),
        }
    }
}

/// 行内差分の結果（旧行・新行それぞれのセグメント列と採用した粒度）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IntraLineDiff {
    /// 採用した粒度（語 / grapheme）。
    pub granularity: Granularity,
    /// 旧行側のセグメント列（削除＝changed が削除部分）。
    pub old_segments: Vec<Segment>,
    /// 新行側のセグメント列（追加＝changed が追加部分）。
    pub new_segments: Vec<Segment>,
}

/// 行に空白区切りの語境界が「ある」とみなすか。
///
/// ASCII 空白で 2 語以上に割れるなら語単位、割れない（＝1 語扱い＝日本語等）なら grapheme 単位。
/// 片側でも語境界が取れなければ安全側で grapheme（細かい方）に倒す（取りこぼし防止＝要件8.2）。
fn pick_granularity(old: &str, new: &str) -> Granularity {
    if has_word_boundary(old) && has_word_boundary(new) {
        Granularity::Word
    } else {
        Granularity::Grapheme
    }
}

/// 内部の空白で 2 つ以上のトークンに割れるか（前後 trim 後に空白を含むか）。
fn has_word_boundary(s: &str) -> bool {
    s.split_whitespace().count() >= 2
}

/// 行を粒度に応じたトークン列へ割る。
///
/// - 語単位: 空白も 1 トークンとして保持し再結合で原文へ戻せるようにする（位置ずれ防止）。
/// - grapheme 単位: Unicode 拡張書記素クラスタ単位（結合文字・絵文字を壊さない）。
fn tokenize(s: &str, g: Granularity) -> Vec<String> {
    match g {
        Granularity::Word => s.split_word_bounds().map(|t| t.to_string()).collect(),
        Granularity::Grapheme => s.graphemes(true).map(|t| t.to_string()).collect(),
    }
}

/// 変更行ペアの行内差分を算出する（要件8.2）。
///
/// 旧行 `old` と新行 `new` を粒度に応じてトークン化し、similar の LCS で
/// 共通部分と変更部分に分けて旧/新それぞれのセグメント列を返す。
/// 連続する同種セグメントは結合し、フロントの描画ノード数を抑える。
pub fn intra_line_segments(old: &str, new: &str) -> IntraLineDiff {
    let granularity = pick_granularity(old, new);
    let old_tokens = tokenize(old, granularity);
    let new_tokens = tokenize(new, granularity);

    let ops = capture_diff_slices(Algorithm::Myers, &old_tokens, &new_tokens);

    let mut old_segments: Vec<Segment> = Vec::new();
    let mut new_segments: Vec<Segment> = Vec::new();

    for op in ops {
        for change in op.iter_changes(&old_tokens, &new_tokens) {
            let text = change.value();
            match change.tag() {
                ChangeTag::Equal => {
                    push_segment(&mut old_segments, false, &text);
                    push_segment(&mut new_segments, false, &text);
                }
                ChangeTag::Delete => push_segment(&mut old_segments, true, &text),
                ChangeTag::Insert => push_segment(&mut new_segments, true, &text),
            }
        }
    }

    IntraLineDiff {
        granularity,
        old_segments,
        new_segments,
    }
}

/// セグメント列の末尾へ追記する。同種（changed が同じ）なら結合してノード数を抑える。
fn push_segment(segments: &mut Vec<Segment>, changed: bool, text: &str) {
    if text.is_empty() {
        return;
    }
    if let Some(last) = segments.last_mut() {
        if last.changed == changed {
            last.text.push_str(text);
            return;
        }
    }
    segments.push(if changed {
        Segment::changed(text)
    } else {
        Segment::equal(text)
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 英文は語境界が取れるので語単位() {
        let r = intra_line_segments("the quick brown fox", "the slow brown fox");
        assert_eq!(r.granularity, Granularity::Word);
        // 共通の "the " と "brown fox" は changed=false、入れ替わった語のみ changed=true。
        let new_changed: Vec<&str> = r
            .new_segments
            .iter()
            .filter(|s| s.changed)
            .map(|s| s.text.as_str())
            .collect();
        assert!(new_changed.iter().any(|t| t.contains("slow")));
        assert!(r
            .new_segments
            .iter()
            .any(|s| !s.changed && s.text.contains("brown")));
    }

    #[test]
    fn 日本語は語境界が取れないので_grapheme_単位へフォールバック() {
        // 空白区切りの語境界が無い行（要件8.2）。文字単位で変わった所だけ強調できる。
        let r = intra_line_segments("今日は晴れです", "今日は雨です");
        assert_eq!(r.granularity, Granularity::Grapheme);
        // 共通の「今日は」「です」は changed=false、「晴れ」→「雨」だけ changed=true。
        let new_changed: String = r
            .new_segments
            .iter()
            .filter(|s| s.changed)
            .map(|s| s.text.clone())
            .collect();
        assert_eq!(new_changed, "雨");
        let old_changed: String = r
            .old_segments
            .iter()
            .filter(|s| s.changed)
            .map(|s| s.text.clone())
            .collect();
        assert_eq!(old_changed, "晴れ");
    }

    #[test]
    fn 片側でも語境界が取れなければ_grapheme_に倒す() {
        // 新行が日本語（語境界なし）→ 安全側で grapheme（細かい方）。
        let r = intra_line_segments("hello world", "こんにちは世界");
        assert_eq!(r.granularity, Granularity::Grapheme);
    }

    #[test]
    fn 共通セグメントは原文へ再結合できる() {
        // 語単位は空白もトークン化して保持するため、new 側の全結合が原文に一致する。
        let r = intra_line_segments("alpha beta gamma", "alpha BETA gamma");
        let rebuilt: String = r.new_segments.iter().map(|s| s.text.clone()).collect();
        assert_eq!(rebuilt, "alpha BETA gamma");
        let old_rebuilt: String = r.old_segments.iter().map(|s| s.text.clone()).collect();
        assert_eq!(old_rebuilt, "alpha beta gamma");
    }

    #[test]
    fn 結合文字を_grapheme_境界で壊さない() {
        // 濁点付き「が」(か+結合濁点)を 1 grapheme として扱い分割しない。
        let old = "か\u{3099}き"; // が(分解) き
        let new = "か\u{3099}く"; // が(分解) く
        let r = intra_line_segments(old, new);
        assert_eq!(r.granularity, Granularity::Grapheme);
        // 共通「が」は壊れず changed=false で残る。
        assert!(r
            .new_segments
            .iter()
            .any(|s| !s.changed && s.text == "か\u{3099}"));
    }
}
