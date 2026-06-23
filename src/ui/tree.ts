// ファイルツリー（要件4.1/4.2・ui-design 5/6章）。role=tree/treeitem の土台＋未読状態マーク。
// sprint 7（design doc 17章）で ARIA 全Web再構築の一部として**キーボード操作性**を本実装する:
// - roving tabindex（ツリー内で常に 1 つだけ tabIndex=0・残りは -1）で Tab 一発でツリーへ入れる。
// - ↑/↓ で treeitem 間移動・Home/End で先頭/末尾・Enter/Space でファイルを開く（要件11.4/11.5
//   「マウスなしで中心フロー完走」＝ツリーが起点）。
// - ディレクトリ treeitem に aria-expanded を付与する（sprint 7 must の明示要求）。
// 未読マーク（± 変更 / ◆ 新規 / 取消線 削除）とフォルダ伝播（淡い ±）の反映は sprint 2 から継続。
import type { TreeEntry } from "../ipc";
import { UNREAD_MARK, type UnreadStore } from "./unread";

const host = () => document.getElementById("tree") as HTMLElement;

/** ツリー直下のエントリを描画する。クリック/Enter/Space でファイルを開く（onOpen 経由）。 */
export function renderTree(
  entries: TreeEntry[],
  onOpen: (entry: TreeEntry) => void,
  unread?: UnreadStore,
): void {
  const ul = host();
  ul.replaceChildren();
  // ツリー自体はキーボードフォーカスを内部の treeitem へ委譲する（roving tabindex）。
  // host の <ul role="tree"> は tabindex を持たず、必ず treeitem の 1 つが tabIndex=0 になる。
  ul.removeAttribute("tabindex");
  for (const entry of entries) {
    ul.appendChild(makeItem(entry, onOpen, unread));
  }
  // roving tabindex の初期化: 先頭 treeitem だけを Tab 到達可能（0）にし、残りは -1。
  initRovingTabindex(ul);
}

function makeItem(
  entry: TreeEntry,
  onOpen: (entry: TreeEntry) => void,
  unread?: UnreadStore,
): HTMLLIElement {
  const li = document.createElement("li");
  li.setAttribute("role", "treeitem");
  li.setAttribute("aria-selected", "false");
  // ディレクトリは aria-expanded を必ず付与する（sprint 7 must）。現状ツリーは 1 段表示で
  // 子の遅延展開は未実装のため折りたたみ（false）として表現する（展開導線は後続で結線）。
  if (entry.is_dir) {
    li.setAttribute("aria-expanded", "false");
  }
  // roving tabindex: 既定は -1。renderTree 末尾で先頭だけ 0 にする。
  li.tabIndex = -1;
  li.dataset.path = entry.path;

  const mark = stateMark(entry, unread);
  // アイコンと名前は別要素にする（F-028: 取り消し線をアイコンまで横切らせない）。
  // 取り消し線はファイル名 span にだけ掛け、アイコン/状態記号は装飾対象から外す。
  // アイコンは絵文字ではなくモノトーン線 SVG（ui-design 6章・案A）。currentColor でテーマ追従し
  // 状態色（青/緑/赤）と干渉させない。symbol スプライト（index.html）を <use> で参照する。
  const iconSpan = document.createElement("span");
  iconSpan.className = "tree-icon";
  iconSpan.setAttribute("aria-hidden", "true"); // 状態はファイル名側 aria-label に集約する。
  iconSpan.appendChild(makeIconSvg(iconIdFor(entry)));
  const nameSpan = document.createElement("span");
  nameSpan.className = "tree-name";
  // 状態マークは色だけに依存しない記号（要件11.5）。読み上げ用に aria-label へも集約する。
  nameSpan.textContent = `${entry.name}${mark.suffix}`;
  if (mark.removed) {
    nameSpan.classList.add("removed"); // 削除済みは取り消し線（ui-design 5章）。span のみに限定。
  }
  li.append(iconSpan, nameSpan);
  applyAriaLabel(li, entry, mark);
  if (mark.propagated) {
    li.dataset.unread = "propagated"; // 伝播マーク（淡 ±）。視覚は CSS で淡色化。
  } else if (mark.suffix) {
    li.dataset.unread = "self";
  }

  // クリックでも開く（マウス操作・従来挙動）。
  li.addEventListener("click", () => {
    selectItem(li);
    if (!entry.is_dir) onOpen(entry);
  });
  // フォーカスを得たら roving tabindex を当該行へ移し選択を反映する（矢印移動の追従）。
  li.addEventListener("focus", () => selectItem(li));
  // キーボード操作（要件11.4/11.5: マウスなしで開く起点に到達）。
  li.addEventListener("keydown", (e) => onItemKeydown(e, li, entry, onOpen));
  return li;
}

// 拡張子→カテゴリ→アイコン symbol id のマッピング（ui-design 6章のカテゴリ集約表）。
// 未知拡張子は generic file（ic-file）へフォールバックする。フォルダは展開/折りたたみに
// かかわらず ic-folder（開閉シェブロンは別タスク）。
const ICON_BY_EXT: Readonly<Record<string, string>> = {
  // コード/マークアップ → file-code（<>）
  ts: "ic-code",
  js: "ic-code",
  html: "ic-code",
  htm: "ic-code",
  xml: "ic-code",
  // データ → braces（{}）
  json: "ic-braces",
  // 設定 → config（sliders）
  yaml: "ic-config",
  yml: "ic-config",
  toml: "ic-config",
  ini: "ic-config",
  // スクリプト → script（terminal）
  sh: "ic-script",
  ps1: "ic-script",
  bat: "ic-script",
  // 画像 → image
  png: "ic-image",
  jpg: "ic-image",
  jpeg: "ic-image",
  gif: "ic-image",
  webp: "ic-image",
  bmp: "ic-image",
  ico: "ic-image",
  svg: "ic-image",
  // テキスト/文書 → file-text
  md: "ic-text",
  markdown: "ic-text",
  txt: "ic-text",
  csv: "ic-text",
  log: "ic-text",
};

/** エントリに対応するアイコン symbol id を返す（フォルダ優先・拡張子マップ・generic フォールバック）。 */
function iconIdFor(entry: TreeEntry): string {
  if (entry.is_dir) return "ic-folder";
  const dot = entry.name.lastIndexOf(".");
  // 先頭ドットのドットファイル（.env 等）や拡張子なしは generic file に倒す。
  if (dot <= 0) return "ic-file";
  const ext = entry.name.slice(dot + 1).toLowerCase();
  return ICON_BY_EXT[ext] ?? "ic-file";
}

/** symbol を参照する <svg><use/></svg> を生成する（SVG なので createElementNS が必須）。 */
function makeIconSvg(iconId: string): SVGSVGElement {
  const ns = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(ns, "svg");
  const use = document.createElementNS(ns, "use");
  // href は SVG2 標準。WebView2(Chromium) は href を解決するため xlink:href は不要。
  use.setAttribute("href", `#${iconId}`);
  svg.appendChild(use);
  return svg;
}

/** 状態マークを aria-label に集約する（色/記号に依存しないテキスト表現＝要件11.5・sprint 7 must）。 */
function applyAriaLabel(
  li: HTMLLIElement,
  entry: TreeEntry,
  mark: { suffix: string; removed: boolean; propagated: boolean },
): void {
  const kind = entry.is_dir ? "フォルダ" : "ファイル";
  let stateText = "";
  if (mark.removed) stateText = "（削除済み）";
  else if (mark.propagated) stateText = "（配下に差分あり）";
  else if (mark.suffix.includes(UNREAD_MARK.created)) stateText = "（新規）";
  else if (mark.suffix.includes(UNREAD_MARK.modified)) stateText = "（差分あり）";
  li.setAttribute("aria-label", `${entry.name} ${kind}${stateText}`);
}

/** roving tabindex の初期化（先頭 treeitem のみ Tab 到達可能・残りは -1）。 */
function initRovingTabindex(ul: HTMLElement): void {
  const items = treeItems(ul);
  items.forEach((it, i) => {
    it.tabIndex = i === 0 ? 0 : -1;
  });
}

/** ツリー内の treeitem 一覧（DOM 順）。 */
function treeItems(ul: HTMLElement): HTMLElement[] {
  return Array.from(ul.querySelectorAll<HTMLElement>('[role="treeitem"]'));
}

/**
 * roving tabindex を当該行に移し選択（aria-selected）を反映する。
 * Tab で戻ってきたとき直前に操作した行へ戻れるよう、tabIndex=0 はツリー内に常に 1 つだけ保つ。
 */
function selectItem(li: HTMLElement): void {
  const ul = host();
  for (const node of treeItems(ul)) {
    node.setAttribute("aria-selected", "false");
    node.tabIndex = -1;
  }
  li.setAttribute("aria-selected", "true");
  li.tabIndex = 0;
}

/** treeitem 上のキー操作（↑/↓/Home/End 移動・Enter/Space で開く＝要件11.4/11.5）。 */
function onItemKeydown(
  e: KeyboardEvent,
  li: HTMLElement,
  entry: TreeEntry,
  onOpen: (entry: TreeEntry) => void,
): void {
  const ul = host();
  const items = treeItems(ul);
  const idx = items.indexOf(li);
  switch (e.key) {
    case "ArrowDown": {
      e.preventDefault();
      focusItemAt(items, Math.min(idx + 1, items.length - 1));
      break;
    }
    case "ArrowUp": {
      e.preventDefault();
      focusItemAt(items, Math.max(idx - 1, 0));
      break;
    }
    case "Home": {
      e.preventDefault();
      focusItemAt(items, 0);
      break;
    }
    case "End": {
      e.preventDefault();
      focusItemAt(items, items.length - 1);
      break;
    }
    case "Enter":
    case " ": {
      // Space/Enter でファイルを開く（要件11.4「マウスなしで中心フロー完走」の起点）。
      e.preventDefault();
      selectItem(li);
      if (!entry.is_dir) onOpen(entry);
      break;
    }
  }
}

/** 指定 index の treeitem へ roving フォーカスを移す。 */
function focusItemAt(items: HTMLElement[], index: number): void {
  const target = items[index];
  if (!target) return;
  selectItem(target);
  target.focus();
}

/** エントリの状態マーク（自身の未読 or フォルダ伝播）を決める。 */
function stateMark(
  entry: TreeEntry,
  unread?: UnreadStore,
): { suffix: string; removed: boolean; propagated: boolean } {
  if (!unread) return { suffix: "", removed: false, propagated: false };
  if (entry.is_dir) {
    // フォルダ自身の未読は子孫からの伝播（淡 ±）。要件4.2: 折りたたみ中でも気づける。
    return unread.folderHasUnread(entry.path)
      ? { suffix: " ±", removed: false, propagated: true }
      : { suffix: "", removed: false, propagated: false };
  }
  const kind = unread.get(entry.path);
  if (!kind) return { suffix: "", removed: false, propagated: false };
  return {
    suffix: ` ${UNREAD_MARK[kind]}`,
    removed: kind === "removed",
    propagated: false,
  };
}
