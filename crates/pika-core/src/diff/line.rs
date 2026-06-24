//! 行差分（similar Myers＋LF 正規化照合・要件8.1/8.2・design doc 7章）。
//!
//! ベースライン内容 `old` と現在内容 `new` の **LF 正規化後**の行を Myers LCS で突き合わせ、
//! unified（インライン）差分の行リストを作る。改行コードのみの差は差分に出さない（要件8.1）。
//! 置換ブロック（削除直後に追加が続く塊）の行ペアには行内差分セグメント（[`crate::diff::inline`]）を
//! 付け、フロントが変更語/grapheme を下線/太字で強調できるようにする（色非依存＝要件8.2/11.5）。

use crate::diff::inline::{intra_line_segments, intraline_too_large, Segment};
use similar::{Algorithm, ChangeTag, TextDiff};

/// 改行コードを LF へ正規化する（CRLF/CR → LF）。
///
/// 未読判定・差分照合はいずれも LF 正規化後の内容で行う（要件8.1）。
/// 改行のみの差を差分に出さないための前段。保存内容そのものは別途原文の改行を維持する。
pub fn normalize_lf(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let bytes = s.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'\r' => {
                out.push('\n');
                if i + 1 < bytes.len() && bytes[i + 1] == b'\n' {
                    i += 1;
                }
            }
            _ => {
                // UTF-8 マルチバイトを壊さないため char 境界で押し込む。
                let ch_len = utf8_char_len(bytes[i]);
                out.push_str(&s[i..i + ch_len]);
                i += ch_len - 1;
            }
        }
        i += 1;
    }
    out
}

/// UTF-8 先頭バイトから char のバイト長を返す（継続バイトは 1 として扱う＝壊さない）。
fn utf8_char_len(b: u8) -> usize {
    if b < 0x80 {
        1
    } else if b >> 5 == 0b110 {
        2
    } else if b >> 4 == 0b1110 {
        3
    } else if b >> 3 == 0b11110 {
        4
    } else {
        1
    }
}

/// 差分行の種別（unified の行頭記号に対応）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DiffTag {
    /// 変更なし（共通行・文脈）。
    Equal,
    /// 追加行（行頭 +・緑）。
    Insert,
    /// 削除行（行頭 -・赤）。
    Delete,
}

/// unified 差分の 1 行（フロントの read-only レンダラが描画する単位）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiffLine {
    /// 種別（行頭 ±記号・色非依存表現の素＝要件8.2/11.5）。
    pub tag: DiffTag,
    /// 旧側の 1 始まり行番号（追加行では `None`）。
    pub old_line_no: Option<usize>,
    /// 新側の 1 始まり行番号（削除行では `None`）。
    pub new_line_no: Option<usize>,
    /// 行本文（改行を含まない）。
    pub content: String,
    /// 行内差分セグメント（置換行のみ。変更語/grapheme を下線/太字で強調＝要件8.2）。
    /// 追加/削除以外（Equal）や、対になる相手がいない純粋な追加/削除では空。
    pub segments: Vec<Segment>,
}

/// ファイル 1 つ分の差分結果。
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct FileDiff {
    /// 差分行リスト（unified・上から順）。
    pub lines: Vec<DiffLine>,
    /// 変更行数（追加＋削除。前後変更ジャンプ F8/Shift+F8 の件数表示に使う）。
    pub change_count: usize,
}

impl FileDiff {
    /// 差分が無い（全行 Equal）か。
    pub fn is_empty_diff(&self) -> bool {
        self.change_count == 0
    }
}

/// ベースライン `old` と現在 `new` の差分を計算する（要件8.1/8.2・design doc 7章）。
///
/// - 比較は **LF 正規化後**で行う（改行のみの差は差分に出さない＝要件8.1）。
/// - 行 LCS は Myers（[`similar`]）。
/// - 置換ブロック（削除と追加が隣接する塊）の行ペアには行内差分セグメントを付与する。
/// - 新規（ベースラインなし）は呼び出し側が `old=""` を渡せば全行追加になる（要件8.1）。
/// - 空ファイル同士は差分なし、片方だけ空は全行追加/削除になる。
pub fn compute_diff(old: &str, new: &str) -> FileDiff {
    let old_norm = normalize_lf(old);
    let new_norm = normalize_lf(new);

    let diff = TextDiff::configure()
        .algorithm(Algorithm::Myers)
        .diff_lines(&old_norm, &new_norm);

    // まず素の行リストを起こす（行内セグメントは後段で置換ペアにだけ付ける）。
    let mut lines: Vec<DiffLine> = Vec::new();
    let mut old_no = 0usize;
    let mut new_no = 0usize;

    for change in diff.iter_all_changes() {
        // similar は行末改行を value に含めるため除去して本文だけ持つ（描画/再結合で改行は付けない）。
        let content = strip_trailing_newline(change.value());
        match change.tag() {
            ChangeTag::Equal => {
                old_no += 1;
                new_no += 1;
                lines.push(DiffLine {
                    tag: DiffTag::Equal,
                    old_line_no: Some(old_no),
                    new_line_no: Some(new_no),
                    content,
                    segments: Vec::new(),
                });
            }
            ChangeTag::Delete => {
                old_no += 1;
                lines.push(DiffLine {
                    tag: DiffTag::Delete,
                    old_line_no: Some(old_no),
                    new_line_no: None,
                    content,
                    segments: Vec::new(),
                });
            }
            ChangeTag::Insert => {
                new_no += 1;
                lines.push(DiffLine {
                    tag: DiffTag::Insert,
                    old_line_no: None,
                    new_line_no: Some(new_no),
                    content,
                    segments: Vec::new(),
                });
            }
        }
    }

    attach_intraline_segments(&mut lines);

    let change_count = lines.iter().filter(|l| l.tag != DiffTag::Equal).count();

    FileDiff {
        lines,
        change_count,
    }
}

/// 行末の改行（LF）を 1 つ取り除く（本文だけを残す）。
fn strip_trailing_newline(s: &str) -> String {
    s.strip_suffix('\n').unwrap_or(s).to_string()
}

/// 置換ブロック（連続する Delete の直後に連続する Insert が続く塊）の行ペアに
/// 行内差分セグメントを付ける。行数が一致する Delete/Insert を上から順に対にする
/// （行数が違う余りは純粋な追加/削除のままセグメント無しで残す）。
fn attach_intraline_segments(lines: &mut [DiffLine]) {
    let mut i = 0;
    while i < lines.len() {
        if lines[i].tag != DiffTag::Delete {
            i += 1;
            continue;
        }
        // 連続 Delete の範囲。
        let del_start = i;
        while i < lines.len() && lines[i].tag == DiffTag::Delete {
            i += 1;
        }
        let del_end = i; // exclusive
                         // 直後に連続する Insert の範囲。
        let ins_start = i;
        while i < lines.len() && lines[i].tag == DiffTag::Insert {
            i += 1;
        }
        let ins_end = i; // exclusive

        // Delete/Insert が隣接していなければ（片方しか無い）置換ではない＝純粋な追加/削除。
        if del_end == del_start || ins_end == ins_start {
            continue;
        }

        // 対にできる本数だけ行内差分を付ける（余りはセグメント無しで残す）。
        let pair = (del_end - del_start).min(ins_end - ins_start);
        for k in 0..pair {
            let old_content = lines[del_start + k].content.clone();
            let new_content = lines[ins_start + k].content.clone();
            // 巨大行（minified JSON 等）の置換ペアは行内差分が O(N·D) で UIスレッド予算を脅かすため、
            // 行内セグメントを付けず純粋な追加/削除に倒す（#39・固まらない方を優先）。
            if intraline_too_large(&old_content, &new_content) {
                continue;
            }
            let intra = intra_line_segments(&old_content, &new_content);
            lines[del_start + k].segments = intra.old_segments;
            lines[ins_start + k].segments = intra.new_segments;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 改行のみの差は差分に出さない() {
        // CRLF vs LF は LF 正規化照合で同一＝差分なし（要件8.1）。
        let d = compute_diff("a\r\nb\r\nc", "a\nb\nc");
        assert!(d.is_empty_diff(), "改行コードのみの差は差分に現れない");
    }

    #[test]
    fn 空ファイル同士は差分なし() {
        let d = compute_diff("", "");
        assert!(d.is_empty_diff());
        assert_eq!(d.lines.len(), 0);
    }

    #[test]
    fn 新規はベースラインなしで全行追加() {
        // old="" を渡すと全行追加（要件8.1 新規＝全行追加）。
        let d = compute_diff("", "x\ny\n");
        assert_eq!(d.change_count, 2);
        assert!(d.lines.iter().all(|l| l.tag == DiffTag::Insert));
    }

    #[test]
    fn 全削除は片方だけ空で全行削除() {
        let d = compute_diff("x\ny\n", "");
        assert_eq!(d.change_count, 2);
        assert!(d.lines.iter().all(|l| l.tag == DiffTag::Delete));
    }

    #[test]
    fn 行番号は旧新で独立に進む() {
        let d = compute_diff("keep\nold\nkeep2\n", "keep\nnew\nkeep2\n");
        // keep(1,1) old(2,-) new(-,2) keep2(3,3)
        let keep2 = d.lines.iter().find(|l| l.content == "keep2").unwrap();
        assert_eq!(keep2.old_line_no, Some(3));
        assert_eq!(keep2.new_line_no, Some(3));
    }

    #[test]
    fn 置換行に行内差分セグメントが付く() {
        let d = compute_diff("the quick fox\n", "the slow fox\n");
        let ins = d.lines.iter().find(|l| l.tag == DiffTag::Insert).unwrap();
        assert!(!ins.segments.is_empty(), "置換行には行内セグメントが付く");
        // 変わった語 slow が changed、共通 fox が non-changed。
        assert!(ins
            .segments
            .iter()
            .any(|s| s.changed && s.text.contains("slow")));
        assert!(ins
            .segments
            .iter()
            .any(|s| !s.changed && s.text.contains("fox")));
    }

    #[test]
    fn 日本語の置換行は文字単位で強調() {
        let d = compute_diff("今日は晴れです\n", "今日は雨です\n");
        let ins = d.lines.iter().find(|l| l.tag == DiffTag::Insert).unwrap();
        let changed: String = ins
            .segments
            .iter()
            .filter(|s| s.changed)
            .map(|s| s.text.clone())
            .collect();
        assert_eq!(changed, "雨");
    }

    #[test]
    fn 巨大行の置換は行内セグメントを付けず純粋な追加削除に倒す() {
        // #39: 改行のない巨大行（minified 想定）の置換ペアは行内差分が O(N·D) で UIスレッドを脅かすため、
        // 行内セグメントを付けず行単位の add/del に倒す（差分自体は出る・固まらない方を優先）。
        use crate::diff::inline::MAX_INTRALINE_GRAPHEMES;
        let old_line = format!("{}X\n", "a".repeat(MAX_INTRALINE_GRAPHEMES + 5));
        let new_line = format!("{}Y\n", "a".repeat(MAX_INTRALINE_GRAPHEMES + 5));
        let d = compute_diff(&old_line, &new_line);
        // 置換（del+ins）として差分は出る。
        assert!(d.lines.iter().any(|l| l.tag == DiffTag::Delete));
        assert!(d.lines.iter().any(|l| l.tag == DiffTag::Insert));
        // ただし巨大行なので行内セグメントは付かない。
        let ins = d.lines.iter().find(|l| l.tag == DiffTag::Insert).unwrap();
        assert!(
            ins.segments.is_empty(),
            "巨大行に行内セグメントが付いた（O(N·D) 爆発の温床）"
        );
        let del = d.lines.iter().find(|l| l.tag == DiffTag::Delete).unwrap();
        assert!(del.segments.is_empty());
    }

    #[test]
    fn 通常サイズの置換行には引き続き行内セグメントが付く() {
        // #39 の閾値が通常行を巻き込まないことの回帰防止。
        let d = compute_diff("the quick fox\n", "the slow fox\n");
        let ins = d.lines.iter().find(|l| l.tag == DiffTag::Insert).unwrap();
        assert!(!ins.segments.is_empty(), "通常行で行内セグメントが消えた");
    }

    #[test]
    fn 純粋な追加だけのブロックには行内セグメントを付けない() {
        // 削除と隣接しない追加は置換ではない＝セグメント無し（誤った行内比較を避ける）。
        let d = compute_diff("a\nc\n", "a\nb\nc\n");
        let ins = d.lines.iter().find(|l| l.tag == DiffTag::Insert).unwrap();
        assert!(ins.segments.is_empty());
    }

    #[test]
    fn 累積差分は前回確認時点から現在まで() {
        // ベースライン（前回確認時点）vs 現在内容＝累積差分（要件8.4）。
        let baseline = "line1\nline2\nline3\n";
        let current = "line1\nLINE2 changed\nline3\nline4 added\n";
        let d = compute_diff(baseline, current);
        // line2 の置換（del+ins）と line4 の追加で計 3 変更行。
        assert_eq!(d.change_count, 3);
    }

    #[test]
    fn 末尾改行有無を本文に混ぜない() {
        let d = compute_diff("a", "a\n");
        // 内容は同じ "a"。LF 正規化後も "a" vs "a\n" は末尾改行差。
        // 本文に改行を混ぜないことを確認（content に \n が無い）。
        assert!(d.lines.iter().all(|l| !l.content.contains('\n')));
    }
}
