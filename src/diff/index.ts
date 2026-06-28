// read-only unified 差分レンダラ（要件8.2・design doc 7章・ui-design 11章）。
//
// Rust 側（pika-core::diff・similar）が算出した hunk を DOM 描画する責務（design doc 7章の線引き）。
// 本レンダラの担当（フロント実装と明記された範囲）:
// - 行頭 +/- 記号（色だけに依存しない弁別＝要件8.2/11.5）
// - 変更語/grapheme の下線/太字（行内セグメントの changed を強調）
// - 前後変更ジャンプ F8/Shift+F8
// - 差分面は読み取り専用（編集は Ctrl+E でソースへ＝呼び出し側が切替）
// - 差分ビュー内検索のハイライト（S5・要件5.4 改訂）：CM6 でないため独自 DOM に別ハイライト機構で当てる

import type { DiffLine, FileDiff, SearchMatch } from "../ipc";

/** 差分行種別ごとの行頭記号（色非依存の弁別＝要件8.2/11.5）。 */
const TAG_MARK: Record<DiffLine["tag"], string> = {
  equal: " ",
  insert: "+",
  delete: "-",
};

/** 正規化済みの行内セグメント（セグメントなし行は行全体を 1 件にして扱う）。 */
interface NormSegment {
  changed: boolean;
  text: string;
}

/** 差分ビュー内検索ヒットの 1 強調範囲（pika-core::diff::DiffHitSpan の UTF-16 写し＝S5）。 */
interface DiffHitSpan {
  /** 行内セグメント番号。 */
  segIndex: number;
  /** セグメント本文内の開始（UTF-16 コードユニット）。 */
  start: number;
  /** 強調する長さ（UTF-16）。 */
  len: number;
  /** 何番目の検索ヒットか（現在ヒット強調・件数対応用）。 */
  hitId: number;
}

/** 差分レンダラのハンドル。変更ジャンプ・検索ハイライト・破棄を提供する。 */
export interface DiffHandle {
  /** 次の変更へジャンプ（F8）。 */
  jumpNext(): void;
  /** 前の変更へジャンプ（Shift+F8）。 */
  jumpPrev(): void;
  /** 変更行数（前後ジャンプ件数・ステータス表示用）。 */
  changeCount(): number;
  /**
   * 差分が表示するテキスト（各行本文を LF で連結したもの）。差分ビュー内検索の検索対象。
   * 差分非対象/変更なし（検索しても意味がない）のときは空文字を返す（S5・要件5.4 改訂）。
   */
  searchText(): string;
  /**
   * 検索ヒット（連結表示テキスト上の UTF-16 オフセット）を差分 DOM 上で強調する（S5）。
   * `current` 番目のヒットを現在ヒット（`diff-search-hit-current`）として一段強める。スクロールはしない。
   */
  setSearchMatches(matches: SearchMatch[], current: number): void;
  /** 指定ヒットを現在ヒットにし、その位置を中央へスクロールする（前後ジャンプ・S5）。 */
  jumpToHit(index: number): void;
  /**
   * 連結表示テキスト上の検索ヒットのうち、差分DOM 上で **1 つ以上の span を生む（描画可能な）** ヒットだけへ
   * 絞り込む（S5・件数と可視ヒットの乖離防止）。行境界の `\n` 等にだけ乗ったヒットは差分DOM に span を作れず
   * Next/Prev で無反応・件数が食い違うため、件数/ジャンプ/ハイライトに数える前に除外する。
   * 判定は computeSpansByLine と同じ重なり規則（各行の本文＝改行を除く content 範囲との重なり）を流用する。
   * 入力の順序を保ったまま部分集合を返す（呼び出し側はこの filtered 配列のインデックスで一貫処理する）。
   */
  filterRenderable(matches: SearchMatch[]): SearchMatch[];
  /** 検索ハイライトを全て消す（バーを閉じた/クエリ空・S5）。 */
  clearSearch(): void;
  /** 破棄（DOM をクリアし keydown ハンドラを外す）。 */
  destroy(): void;
}

/**
 * unified 差分をホスト要素へ描画する（読み取り専用＝要件8.2）。
 * @param host 描画先 DOM 要素
 * @param diff Rust 算出の差分
 */
export function renderDiff(host: HTMLElement, diff: FileDiff): DiffHandle {
  host.replaceChildren();
  host.classList.add("diff-view");
  // 差分面は読み取り専用（編集は Ctrl+E でソースへ＝要件8.2）。
  host.setAttribute("role", "document");
  host.setAttribute("aria-readonly", "true");
  host.setAttribute("aria-label", `差分（変更 ${diff.change_count} 件）`);

  if (!diff.has_baseline_content) {
    // ハッシュのみ記録（10MB以上/画像/機密）は差分非対象（要件8.2/9.2）。
    const note = document.createElement("p");
    note.className = "diff-unavailable";
    note.textContent = "このファイルは差分非対象です（10MB以上 / 画像 / 機密ファイル）。";
    host.appendChild(note);
    return noopHandle();
  }

  if (diff.change_count === 0) {
    const note = document.createElement("p");
    note.className = "diff-empty";
    note.textContent = "前回確認時点から変更はありません。";
    host.appendChild(note);
    return noopHandle();
  }

  const container = document.createElement("div");
  container.className = "diff-lines";
  // 変更行（insert/delete）の DOM 参照をジャンプ用に集める。
  const changeRows: HTMLElement[] = [];
  // 行ごとの本文要素・正規化セグメントを検索ハイライトの再描画用に保持する（S5）。
  const bodyEls: HTMLElement[] = [];
  const lineSegs: NormSegment[][] = [];
  // 連結表示テキスト上の各行頭 UTF-16 オフセット（検索ヒットの行/セグメント割り当て用・S5）。
  const lineStarts: number[] = [];
  let textCursor = 0;
  const textParts: string[] = [];

  diff.lines.forEach((line, i) => {
    const { row, body, segs } = renderLine(line);
    container.appendChild(row);
    bodyEls.push(body);
    lineSegs.push(segs);
    if (line.tag !== "equal") {
      changeRows.push(row);
    }
    // 表示テキスト連結（行は LF 区切り・末尾改行なし＝pika-core::diff::diff_display_text と一致）。
    if (i > 0) {
      textCursor += 1; // 行境界の '\n'。
      textParts.push("\n");
    }
    lineStarts.push(textCursor);
    textParts.push(line.content);
    textCursor += line.content.length;
  });
  host.appendChild(container);
  const displayText = textParts.join("");

  // 前後変更ジャンプ（F8/Shift+F8）。連続する変更ブロックは塊ごとに飛ぶ。
  const blocks = groupBlocks(changeRows);
  let cursor = -1;

  function focusBlock(idx: number): void {
    if (blocks.length === 0) return;
    cursor = ((idx % blocks.length) + blocks.length) % blocks.length;
    const target = blocks[cursor];
    target.scrollIntoView({ block: "center" });
    // 一時ハイライト（aria には現在位置を出す）。
    for (const b of blocks) b.classList.remove("diff-block-current");
    target.classList.add("diff-block-current");
    host.setAttribute(
      "aria-label",
      `差分（変更 ${diff.change_count} 件・ブロック ${cursor + 1}/${blocks.length}）`,
    );
  }

  const onKey = (e: KeyboardEvent): void => {
    if (e.key === "F8") {
      e.preventDefault();
      if (e.shiftKey) focusBlock(cursor - 1);
      else focusBlock(cursor + 1);
    }
  };
  host.addEventListener("keydown", onKey);
  // フォーカスを受けられるようにして F8 を拾う（読み取り専用のまま）。
  host.tabIndex = 0;

  // ── 差分ビュー内検索ハイライト（S5・要件5.4 改訂）──────────────────────────────
  // pika-core::diff::map_matches_to_spans の **UTF-16 写し**。連結表示テキスト上の検索ヒットを
  // 行・セグメント単位の強調範囲へ割り当てる（乖離防止のため Rust 側が正本＝cargo test で固める）。
  let spansByLine = new Map<number, DiffHitSpan[]>();
  let hitLineSet = new Set<number>();
  let searchMatches: SearchMatch[] = [];
  let currentHitId = -1;

  /**
   * 検索ヒット [ms,me)（連結表示テキスト上の UTF-16）と li 行の本文範囲（改行を除く content）の重なりを
   * **行ローカル offset** で返す（重ならなければ null）。computeSpansByLine と filterRenderable が同じ重なり
   * 規則を共有するための単一源（S5 で Rust 側が正本＝乖離防止）。
   */
  function lineContentOverlap(
    ms: number,
    me: number,
    li: number,
  ): { localS: number; localE: number } | null {
    const lineStart = lineStarts[li];
    const lineEnd = lineStart + diff.lines[li].content.length;
    const os = Math.max(ms, lineStart);
    const oe = Math.min(me, lineEnd);
    if (oe <= os) return null;
    return { localS: os - lineStart, localE: oe - lineStart };
  }

  function computeSpansByLine(matches: SearchMatch[]): Map<number, DiffHitSpan[]> {
    const map = new Map<number, DiffHitSpan[]>();
    matches.forEach((m, hitId) => {
      const ms = m.utf16_start;
      const me = m.utf16_end;
      if (me <= ms) return;
      for (let li = 0; li < lineSegs.length; li++) {
        const ov = lineContentOverlap(ms, me, li);
        if (!ov) continue;
        const { localS, localE } = ov;
        const segs = lineSegs[li];
        let segOff = 0;
        for (let j = 0; j < segs.length; j++) {
          const segStart = segOff;
          const segLen = segs[j].text.length;
          const segEnd = segStart + segLen;
          segOff = segEnd;
          const ss = Math.max(localS, segStart);
          const se = Math.min(localE, segEnd);
          if (se <= ss) continue;
          let arr = map.get(li);
          if (!arr) {
            arr = [];
            map.set(li, arr);
          }
          arr.push({ segIndex: j, start: ss - segStart, len: se - ss, hitId });
        }
      }
    });
    return map;
  }

  /** 1 行の本文を、検索ヒットの強調を織り込んで再描画する（ヒットなしなら素の段落へ戻る）。 */
  function renderLineBody(li: number): void {
    const body = bodyEls[li];
    body.replaceChildren();
    const segs = lineSegs[li];
    const lineSpans = spansByLine.get(li);
    segs.forEach((seg, segIndex) => {
      const hits = lineSpans
        ? lineSpans
            .filter((s) => s.segIndex === segIndex)
            .sort((a, b) => a.start - b.start)
        : [];
      if (hits.length === 0) {
        appendRun(body, seg.text, seg.changed, false, false);
        return;
      }
      let pos = 0;
      for (const h of hits) {
        if (h.start > pos) {
          appendRun(body, seg.text.slice(pos, h.start), seg.changed, false, false);
        }
        appendRun(
          body,
          seg.text.slice(h.start, h.start + h.len),
          seg.changed,
          true,
          h.hitId === currentHitId,
        );
        pos = h.start + h.len;
      }
      if (pos < seg.text.length) {
        appendRun(body, seg.text.slice(pos), seg.changed, false, false);
      }
    });
  }

  function setSearchMatches(matches: SearchMatch[], current: number): void {
    const newMap = computeSpansByLine(matches);
    const newLines = new Set(newMap.keys());
    spansByLine = newMap;
    searchMatches = matches;
    currentHitId = current;
    // 以前ヒットがあった行（消す対象）と新たにヒットが乗る行の和集合だけ描き直す。
    const toRebuild = new Set<number>([...hitLineSet, ...newLines]);
    for (const li of toRebuild) renderLineBody(li);
    hitLineSet = newLines;
  }

  function jumpToHit(index: number): void {
    if (index < 0 || index >= searchMatches.length) return;
    if (index !== currentHitId) {
      const prev = currentHitId;
      currentHitId = index;
      // 旧現在ヒット・新現在ヒットを含む行だけ -current クラスを付け替える。
      const affected = new Set<number>();
      for (const [li, spans] of spansByLine) {
        if (spans.some((s) => s.hitId === prev || s.hitId === index)) affected.add(li);
      }
      for (const li of affected) renderLineBody(li);
    }
    const el = container.querySelector<HTMLElement>(".diff-search-hit-current");
    if (el) el.scrollIntoView({ block: "center" });
  }

  function clearSearch(): void {
    if (hitLineSet.size === 0) return;
    const prev = hitLineSet;
    spansByLine = new Map();
    searchMatches = [];
    currentHitId = -1;
    hitLineSet = new Set();
    for (const li of prev) renderLineBody(li);
  }

  /**
   * 描画可能（≥1 span を生む）ヒットだけへ絞る（S5・件数乖離防止）。computeSpansByLine と同じ重なり規則で、
   * いずれかの行の本文範囲（改行を除く content 部分）と重なるヒットだけ残す（セグメントまで割らずとも、
   * 行 content はセグメントで隙間なく敷き詰められているので行 content との重なり＝必ず 1 セグメントと重なる）。
   * 行境界の `\n` にだけ乗ったヒット（どの行 content とも重ならない）は span を作れないので除外する。
   */
  function filterRenderable(matches: SearchMatch[]): SearchMatch[] {
    return matches.filter((m) => {
      const ms = m.utf16_start;
      const me = m.utf16_end;
      if (me <= ms) return false;
      for (let li = 0; li < diff.lines.length; li++) {
        // 本文範囲と重なる＝この行に span を作れる（computeSpansByLine と同じ重なり規則を流用）。
        if (lineContentOverlap(ms, me, li)) return true;
      }
      return false; // どの行 content とも重ならない（改行/ゼロ幅のみ）＝描画不可。
    });
  }

  return {
    jumpNext: () => focusBlock(cursor + 1),
    jumpPrev: () => focusBlock(cursor - 1),
    changeCount: () => diff.change_count,
    searchText: () => displayText,
    setSearchMatches,
    jumpToHit,
    filterRenderable,
    clearSearch,
    destroy: () => {
      host.removeEventListener("keydown", onKey);
      host.replaceChildren();
      host.classList.remove("diff-view");
    },
  };
}

/** 本文へ 1 ラン（テキスト断片）を追記する。変更/検索ヒットの強調だけ span 化しノード数を抑える。 */
function appendRun(
  parent: HTMLElement,
  text: string,
  changed: boolean,
  hit: boolean,
  current: boolean,
): void {
  if (text === "") return;
  if (!changed && !hit) {
    // 地の文（変更でも検索ヒットでもない）はテキストノードで軽く描く。
    parent.appendChild(document.createTextNode(text));
    return;
  }
  const s = document.createElement("span");
  // 変更語/grapheme は下線＋太字（色だけに依存しない＝要件8.2/11.5）。
  if (changed) s.className = "diff-seg-changed";
  if (hit) {
    s.classList.add("diff-search-hit");
    // 現在ヒットは枠で 1 件だけ強める（背景色だけに依存しない＝S5）。
    if (current) s.classList.add("diff-search-hit-current");
  }
  s.textContent = text;
  parent.appendChild(s);
}

/** 差分 1 行を描画する（行頭記号＋行番号＋行内セグメント）。本文要素と正規化セグメントも返す。 */
function renderLine(line: DiffLine): {
  row: HTMLElement;
  body: HTMLElement;
  segs: NormSegment[];
} {
  const row = document.createElement("div");
  row.className = `diff-line diff-${line.tag}`;
  // 色だけに依存しないため data 属性に記号意味も持たせる（forced-colors 時の弁別）。
  row.dataset.tag = line.tag;

  // 旧/新行番号（gutter）。
  const gutter = document.createElement("span");
  gutter.className = "diff-gutter";
  gutter.setAttribute("aria-hidden", "true");
  gutter.textContent = `${fmtNo(line.old_line_no)} ${fmtNo(line.new_line_no)}`;
  row.appendChild(gutter);

  // 行頭 +/- 記号（色非依存の弁別＝要件8.2/11.5）。
  const mark = document.createElement("span");
  mark.className = "diff-mark";
  mark.textContent = TAG_MARK[line.tag];
  row.appendChild(mark);

  // 正規化セグメント（セグメントなし行＝置換でない行は行全体を 1 件にして検索ハイライトと統一する）。
  const segs: NormSegment[] =
    line.segments.length > 0
      ? line.segments.map((s) => ({ changed: s.changed, text: s.text }))
      : [{ changed: false, text: line.content }];

  // 本文（行内セグメント・検索ヒットは setSearchMatches が後から織り込む）。
  const body = document.createElement("span");
  body.className = "diff-body";
  for (const seg of segs) {
    appendRun(body, seg.text, seg.changed, false, false);
  }
  row.appendChild(body);

  // スクリーンリーダー向けに行の意味をテキスト化（aria-label）。
  const tagWord =
    line.tag === "insert" ? "追加" : line.tag === "delete" ? "削除" : "変更なし";
  row.setAttribute("aria-label", `${tagWord}: ${line.content}`);
  return { row, body, segs };
}

/** 行番号を右詰め 4 桁相当で整形（未設定は空欄）。 */
function fmtNo(n: number | undefined): string {
  return n === undefined ? "    " : String(n).padStart(4, " ");
}

/** 連続する変更行を 1 ブロックにまとめる（F8 ジャンプ単位）。先頭行を代表にする。 */
function groupBlocks(changeRows: HTMLElement[]): HTMLElement[] {
  const blocks: HTMLElement[] = [];
  let prev: HTMLElement | null = null;
  for (const row of changeRows) {
    if (prev && prev.nextElementSibling === row) {
      // 直前の変更行と隣接＝同ブロック（代表は変えない）。
      prev = row;
      continue;
    }
    blocks.push(row);
    prev = row;
  }
  return blocks;
}

/** 変更が無い/差分非対象のときの no-op ハンドル。 */
function noopHandle(): DiffHandle {
  return {
    jumpNext: () => {},
    jumpPrev: () => {},
    changeCount: () => 0,
    searchText: () => "",
    setSearchMatches: () => {},
    jumpToHit: () => {},
    filterRenderable: (m) => m,
    clearSearch: () => {},
    destroy: () => {},
  };
}
