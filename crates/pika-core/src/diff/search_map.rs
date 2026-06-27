//! 差分ビュー内検索のヒット位置マッピング（S5・要件5.4 改訂・design doc 7章）。
//!
//! 差分の独自 DOM レンダラ（フロント `src/diff/index.ts`）は CodeMirror6 ではないため、CM6 の
//! 検索ハイライト（StateField）を流用できない。そこで「差分が表示するテキスト」（各差分行の本文を
//! LF で連結したもの）に対して [`crate::search`] で全文検索し、得られたヒット（**byte 半開区間**の
//! [`Match`]）を「**どの差分行の・何番目セグメントの・セグメント内どこか**」へ写してから別ハイライト
//! 機構で強調する。本モジュールはその **純粋変換**（cargo test の決定論ゲート対象）で、フロント TS
//! （`src/diff/index.ts` 内の写し）はこの規則を **UTF-16 コードユニット**で 1:1 ミラーする
//! （`shortcuts.rs`↔`shortcuts.ts` と同じパリティ契約。乖離防止のため本書を正本とする）。
//!
//! ポイント:
//! - 連結は各行 `content` を `\n` で結合する（末尾改行は付けない）。検索はこの文字列を対象にする。
//! - セグメントを持たない行（Equal・対のない純粋な追加/削除）は **行全体を 1 セグメント**（`seg_index = 0`）
//!   として扱う（フロント `renderLine` の描画と同じ正規化）。
//! - 1 ヒットが行/セグメントを跨ぐと複数の [`DiffHitSpan`] に分割される（行境界の `\n` 自体は
//!   どのセグメントにも属さないため強調しない）。
//! - LF 正規化済みの行本文・grapheme 単位差分セグメントいずれも byte 境界で扱うため、返す byte
//!   オフセットは常に char 境界に乗る（マルチバイト文字を割らない）。

use crate::diff::line::DiffLine;
use crate::search::Match;

/// 差分が表示するテキスト（各差分行の本文を LF で連結）を組み立てる。
///
/// 検索はこの文字列に対して行い、ヒットの byte オフセットを [`map_matches_to_spans`] が
/// 行/セグメントへ戻す。末尾改行は付けない（行数 N に対し改行は N-1 個）。
pub fn diff_display_text(lines: &[DiffLine]) -> String {
    let mut s = String::new();
    for (i, line) in lines.iter().enumerate() {
        if i > 0 {
            s.push('\n');
        }
        s.push_str(&line.content);
    }
    s
}

/// 差分 DOM 上の 1 ハイライト範囲（1 ヒットが行/セグメントを跨ぐと複数生成される）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DiffHitSpan {
    /// 対象差分行（`lines` の index）。
    pub line_index: usize,
    /// 行内セグメント番号（セグメントを持たない行は `0`＝行全体）。
    pub seg_index: usize,
    /// セグメント本文内の開始 byte（含む・char 境界）。
    pub start: usize,
    /// 強調する byte 長（`start + len` も char 境界）。
    pub len: usize,
}

/// 連結表示テキスト上の検索ヒット（byte 半開区間）を、差分行・セグメント単位のハイライト範囲へ
/// 変換する（S5・要件5.4 改訂）。
///
/// 返り値は `matches` と同じ並び・同じ件数で、各ヒットに対応する span 列（文書順）を持つ。
/// ヒットが空（`start == end`）や、行本文に重ならない（改行のみに乗る）場合は空 span 列になる。
pub fn map_matches_to_spans(lines: &[DiffLine], matches: &[Match]) -> Vec<Vec<DiffHitSpan>> {
    // 各差分行の「表示テキスト内 byte 開始位置」「行本文の byte 長」「行内セグメントの byte 区間」を
    // 前計算する（マッチごとに全行を再走査しても済むよう軽量な索引を持つ）。
    struct LineInfo {
        /// 表示テキスト内の行頭 byte。
        start: usize,
        /// 行本文の byte 長（改行は含まない）。
        len: usize,
        /// 行内セグメントの (開始 byte（行内相対）, byte 長) 列。空行も最低 1 件持つ。
        segs: Vec<(usize, usize)>,
    }

    let mut infos: Vec<LineInfo> = Vec::with_capacity(lines.len());
    let mut cursor = 0usize;
    for (i, line) in lines.iter().enumerate() {
        if i > 0 {
            // 連結の '\n' 1 byte 分（行境界）。
            cursor += 1;
        }
        let content_len = line.content.len();
        let mut segs: Vec<(usize, usize)> = Vec::new();
        if line.segments.is_empty() {
            // セグメントなし行は行全体を擬似セグメント 0 として扱う（フロント描画と一致）。
            segs.push((0, content_len));
        } else {
            let mut off = 0usize;
            for seg in &line.segments {
                let l = seg.text.len();
                segs.push((off, l));
                off += l;
            }
        }
        infos.push(LineInfo {
            start: cursor,
            len: content_len,
            segs,
        });
        cursor += content_len;
    }

    let mut out: Vec<Vec<DiffHitSpan>> = Vec::with_capacity(matches.len());
    for m in matches {
        let mut spans: Vec<DiffHitSpan> = Vec::new();
        let (ms, me) = (m.start, m.end);
        if me <= ms {
            // 空ヒットは強調しない（空 span 列を返して件数の対応を保つ）。
            out.push(spans);
            continue;
        }
        for (li, info) in infos.iter().enumerate() {
            let line_start = info.start;
            let line_end = info.start + info.len;
            // 行本文とヒット範囲の重なり（改行のみに乗る部分はここで自然に落ちる）。
            let os = ms.max(line_start);
            let oe = me.min(line_end);
            if oe <= os {
                continue;
            }
            // 行内相対へ落とす。
            let local_s = os - line_start;
            let local_e = oe - line_start;
            // 重なりをセグメント単位へ割る（変更/非変更セグメントを跨ぐと複数 span）。
            for (j, &(seg_off, seg_len)) in info.segs.iter().enumerate() {
                let seg_start = seg_off;
                let seg_end = seg_off + seg_len;
                let ss = local_s.max(seg_start);
                let se = local_e.min(seg_end);
                if se <= ss {
                    continue;
                }
                spans.push(DiffHitSpan {
                    line_index: li,
                    seg_index: j,
                    start: ss - seg_start,
                    len: se - ss,
                });
            }
        }
        out.push(spans);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::diff::inline::Segment;
    use crate::diff::line::{compute_diff, DiffTag};
    use crate::search::{search_all, Cancel, SearchOptions};

    fn opts_literal() -> SearchOptions {
        SearchOptions {
            case_sensitive: true,
            whole_word: false,
            regex: false,
        }
    }

    /// span が指す実際の強調文字列を組み立てる（マッピングの正しさを内容で検証する）。
    fn span_text(lines: &[DiffLine], span: &DiffHitSpan) -> String {
        let line = &lines[span.line_index];
        let seg_text: &str = if line.segments.is_empty() {
            &line.content
        } else {
            &line.segments[span.seg_index].text
        };
        seg_text[span.start..span.start + span.len].to_string()
    }

    /// ヒット 1 件分の span 列を連結して、検索がヒットした文字列に一致するかを返す。
    fn joined(lines: &[DiffLine], spans: &[DiffHitSpan]) -> String {
        spans.iter().map(|s| span_text(lines, s)).collect()
    }

    #[test]
    fn 差分表示テキストは行を改行で連結する() {
        let d = compute_diff("a\nb\n", "a\nB\n");
        // a(equal) b(delete) B(insert) の 3 行 → "a\nb\nB"（末尾改行なし）。
        let text = diff_display_text(&d.lines);
        assert_eq!(text, "a\nb\nB");
    }

    #[test]
    fn ascii_ヒットを正しい行とセグメントへ写す() {
        // equal 行に検索語があるケース（セグメントなし行＝擬似セグメント0）。
        let d = compute_diff("keep foo\nold\n", "keep foo\nnew\n");
        let text = diff_display_text(&d.lines);
        let r = search_all(&text, "foo", opts_literal(), &Cancel::new()).unwrap();
        assert_eq!(r.matches.len(), 1);
        let spans = map_matches_to_spans(&d.lines, &r.matches);
        assert_eq!(spans.len(), 1);
        assert_eq!(spans[0].len(), 1, "1 行 1 セグメントに収まる");
        let s = spans[0][0];
        // 1 行目（line_index 0）の "keep foo" の "foo"（byte 5..8）。
        assert_eq!(s.line_index, 0);
        assert_eq!(s.seg_index, 0);
        assert_eq!(span_text(&d.lines, &s), "foo");
    }

    #[test]
    fn 後続行のヒットは改行ぶんずれて正しい行へ写る() {
        // LF 連結のオフセット計算が行をまたいでも崩れないこと（line_index が +1 にならない）。
        let d = compute_diff("alpha\nbeta\n", "alpha\nbeta\ngamma\n");
        let text = diff_display_text(&d.lines);
        let r = search_all(&text, "gamma", opts_literal(), &Cancel::new()).unwrap();
        assert_eq!(r.matches.len(), 1);
        let spans = map_matches_to_spans(&d.lines, &r.matches);
        let s = spans[0][0];
        // gamma は 3 行目（追加行）。content == "gamma" 全体。
        assert_eq!(s.line_index, 2);
        assert_eq!(span_text(&d.lines, &s), "gamma");
    }

    #[test]
    fn 日本語ヒットは_byte_オフセットでも文字を壊さない() {
        // grapheme 単位差分セグメント（日本語）の上でも byte 境界が char 境界に乗ること。
        let d = compute_diff("今日は晴れです\n", "今日は雨です\n");
        let text = diff_display_text(&d.lines);
        // "は雨" は insert 行のセグメント境界（共通「今日は」と変更「雨」）を跨ぐ。
        let r = search_all(&text, "は雨", opts_literal(), &Cancel::new()).unwrap();
        assert_eq!(r.matches.len(), 1, "insert 行にのみヒットする");
        let spans = map_matches_to_spans(&d.lines, &r.matches);
        // 連結すると検索語に一致する（マルチバイトでも割れない）。
        assert_eq!(joined(&d.lines, &spans[0]), "は雨");
        // insert 行（DiffTag::Insert）に乗っていること。
        let insert_idx = d
            .lines
            .iter()
            .position(|l| l.tag == DiffTag::Insert)
            .unwrap();
        assert!(spans[0].iter().all(|s| s.line_index == insert_idx));
    }

    #[test]
    fn ヒットがセグメントを跨ぐと複数_span_になる() {
        // 変更セグメントと非変更セグメントの境界を跨ぐヒット → 2 span（決定論のため手組み）。
        let lines = vec![DiffLine {
            tag: DiffTag::Insert,
            old_line_no: None,
            new_line_no: Some(1),
            content: "fooBAR baz".to_string(),
            segments: vec![
                Segment {
                    changed: true,
                    text: "fooBAR".to_string(),
                },
                Segment {
                    changed: false,
                    text: " baz".to_string(),
                },
            ],
        }];
        let text = diff_display_text(&lines);
        assert_eq!(text, "fooBAR baz");
        // "BAR ba" は seg0("fooBAR") の末尾と seg1(" baz") の先頭を跨ぐ。
        let r = search_all(&text, "BAR ba", opts_literal(), &Cancel::new()).unwrap();
        let spans = map_matches_to_spans(&lines, &r.matches);
        assert_eq!(spans.len(), 1);
        assert_eq!(spans[0].len(), 2, "セグメント境界で 2 つに割れる");
        assert_eq!(spans[0][0].seg_index, 0);
        assert_eq!(spans[0][1].seg_index, 1);
        // 連結すると検索語に戻る（取りこぼし/重複なし）。
        assert_eq!(joined(&lines, &spans[0]), "BAR ba");
    }

    #[test]
    fn 複数ヒットは入力と同じ並びで返る() {
        let d = compute_diff("foo foo\n", "foo foo foo\n");
        let text = diff_display_text(&d.lines);
        let r = search_all(&text, "foo", opts_literal(), &Cancel::new()).unwrap();
        let spans = map_matches_to_spans(&d.lines, &r.matches);
        assert_eq!(spans.len(), r.matches.len());
        // すべてのヒットがいずれかの行・セグメントへ写る（空 span が無い）。
        assert!(spans.iter().all(|hit| hit.len() == 1));
        assert!(spans
            .iter()
            .all(|hit| span_text(&d.lines, &hit[0]) == "foo"));
    }

    #[test]
    fn 空ヒットと空入力は空_span_を返す() {
        let d = compute_diff("a\n", "b\n");
        // 入力 matches が空。
        assert!(map_matches_to_spans(&d.lines, &[]).is_empty());
        // start==end の空ヒットは空 span 列（件数対応は保つ）。
        let spans = map_matches_to_spans(&d.lines, &[Match { start: 0, end: 0 }]);
        assert_eq!(spans.len(), 1);
        assert!(spans[0].is_empty());
    }
}
