// read-only unified 差分レンダラ（要件8.2・design doc 7章・ui-design 11章）。
//
// Rust 側（pika-core::diff・similar）が算出した hunk を DOM 描画する責務（design doc 7章の線引き）。
// 本レンダラの担当（フロント実装と明記された範囲）:
// - 行頭 +/- 記号（色だけに依存しない弁別＝要件8.2/11.5）
// - 変更語/grapheme の下線/太字（行内セグメントの changed を強調）
// - 前後変更ジャンプ F8/Shift+F8
// - 差分面は読み取り専用（編集は Ctrl+E でソースへ＝呼び出し側が切替）

import type { DiffLine, FileDiff } from "../ipc";

/** 差分行種別ごとの行頭記号（色非依存の弁別＝要件8.2/11.5）。 */
const TAG_MARK: Record<DiffLine["tag"], string> = {
  equal: " ",
  insert: "+",
  delete: "-",
};

/** 差分レンダラのハンドル。変更ジャンプ・破棄を提供する。 */
export interface DiffHandle {
  /** 次の変更へジャンプ（F8）。 */
  jumpNext(): void;
  /** 前の変更へジャンプ（Shift+F8）。 */
  jumpPrev(): void;
  /** 変更行数（前後ジャンプ件数・ステータス表示用）。 */
  changeCount(): number;
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

  for (const line of diff.lines) {
    const row = renderLine(line);
    container.appendChild(row);
    if (line.tag !== "equal") {
      changeRows.push(row);
    }
  }
  host.appendChild(container);

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

  return {
    jumpNext: () => focusBlock(cursor + 1),
    jumpPrev: () => focusBlock(cursor - 1),
    changeCount: () => diff.change_count,
    destroy: () => {
      host.removeEventListener("keydown", onKey);
      host.replaceChildren();
      host.classList.remove("diff-view");
    },
  };
}

/** 差分 1 行を描画する（行頭記号＋行番号＋行内セグメント）。 */
function renderLine(line: DiffLine): HTMLElement {
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

  // 本文（置換行は行内セグメント・それ以外はプレーン）。
  const body = document.createElement("span");
  body.className = "diff-body";
  if (line.segments.length > 0) {
    for (const seg of line.segments) {
      const s = document.createElement("span");
      s.textContent = seg.text;
      if (seg.changed) {
        // 変更語/grapheme は下線＋太字（色だけに依存しない＝要件8.2/11.5）。
        s.className = "diff-seg-changed";
      }
      body.appendChild(s);
    }
  } else {
    body.textContent = line.content;
  }
  row.appendChild(body);

  // スクリーンリーダー向けに行の意味をテキスト化（aria-label）。
  const tagWord =
    line.tag === "insert" ? "追加" : line.tag === "delete" ? "削除" : "変更なし";
  row.setAttribute("aria-label", `${tagWord}: ${line.content}`);
  return row;
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
    destroy: () => {},
  };
}
