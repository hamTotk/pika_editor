// ファイルツリー（要件4.1/4.2・ui-design 5/6/7章）。role=tree/treeitem の土台＋未読状態マーク。
// sprint 7（design doc 17章）で ARIA 全Web再構築の一部として**キーボード操作性**を本実装した:
// - roving tabindex（ツリー内で常に 1 つだけ tabIndex=0・残りは -1）で Tab 一発でツリーへ入れる。
// - ↑/↓ で treeitem 間移動・Home/End で先頭/末尾・Enter/Space でファイルを開く（要件11.4/11.5
//   「マウスなしで中心フロー完走」＝ツリーが起点）。
// - ディレクトリ treeitem に aria-expanded を付与する（sprint 7 must の明示要求）。
// UI ブラッシュアップ T2（ui-design 7章）で **フォルダ開閉シェブロン（▾/▸）＋段階インデント展開**を実装:
// - フォルダ行の先頭にトグル（▾=展開 / ▸=折りたたみ）を出し、ファイル行は同幅の空きでインデント整列する。
// - フォルダ（シェブロン/行）クリックまたは Enter/Space/ArrowRight で子を ipc 経由で遅延取得し、その行の
//   直後に深さ+1 で挿入する。再度トグルで折りたたむ。展開済みパス集合と取得済み子キャッシュは本モジュール
//   内に保持し、外部変更等での再構築でも展開状態を保つ。aria-expanded は実値（true/false）に連動する。
// - 段階インデントは深さに応じて行の左パディングを増やす（base + depth * 段差。CSS の calc で算出）。
// 未読マーク（± 変更 / ◆ 新規 / 取消線 削除）とフォルダ伝播（淡い ±）の反映は sprint 2 から継続。
// 絵文字は使わない（ui-design 7章: シェブロン＋種別アイコン＋名前＋状態マーク）。
import type { TreeEntry } from "../ipc";
import { UNREAD_MARK, type UnreadStore } from "./unread";
import { pathKey } from "../util/path";
import { applyRovingTabindex, collectByRole } from "./roving";

const host = () => document.getElementById("tree") as HTMLElement;

/** 子フォルダを ipc 経由で遅延取得する関数（副作用なし列挙＝ipc.listDir を渡す想定）。 */
type FetchChildren = (path: string) => Promise<TreeEntry[]>;

// パス区切りの正規化（Windows の \ と / を吸収し展開集合/キャッシュのキーを安定させる・**大小保持**）は
// util/path.pathKey を使う（backend 索引キー突合のため大小は潰さない）。

/**
 * ツリーの描画状態（再構築をまたいで保持する単一源）。
 * renderTree のたびに rootEntries/onOpen/fetch/unread を更新し、expanded/childCache は維持する。
 */
const treeState = {
  rootEntries: [] as TreeEntry[],
  onOpen: (_entry: TreeEntry) => {},
  fetch: undefined as FetchChildren | undefined,
  unread: undefined as UnreadStore | undefined,
  /** 右クリック（コンテキストメニュー）で新規作成/削除を出す（要件11・T5）。未指定なら何もしない。 */
  onContextMenu: undefined as
    | ((entry: TreeEntry, x: number, y: number) => void)
    | undefined,
  /** 展開済みフォルダの正規化パス集合（再構築でも展開を保つ）。 */
  expanded: new Set<string>(),
  /** 取得済み子のキャッシュ（正規化パス→直下エントリ）。展開は同期的にここから組む。 */
  childCache: new Map<string, TreeEntry[]>(),
  /** 再描画後にフォーカスを戻す対象パス（キーボード展開/折りたたみの追従用）。 */
  focusAfterRender: undefined as string | undefined,
};

/**
 * ツリーを描画する。クリック/Enter/Space でファイルを開き（onOpen）、フォルダはトグルで開閉する。
 *
 * @param entries 直下（ルート）のエントリ。
 * @param onOpen ファイルを開くコールバック（フォルダでは呼ばない）。
 * @param fetchChildren 子フォルダの遅延取得（副作用なし列挙）。未指定なら展開不可（直下のみ）。
 * @param unread 未読ストア（状態マーク/伝播）。
 */
export function renderTree(
  entries: TreeEntry[],
  onOpen: (entry: TreeEntry) => void,
  fetchChildren?: FetchChildren,
  unread?: UnreadStore,
  onContextMenu?: (entry: TreeEntry, x: number, y: number) => void,
): void {
  treeState.rootEntries = entries;
  treeState.onOpen = onOpen;
  treeState.fetch = fetchChildren;
  treeState.unread = unread;
  treeState.onContextMenu = onContextMenu;
  rebuild();
}

/**
 * 指定フォルダの子を再取得してツリーへ反映する（新規作成/削除後の差分更新・T5）。
 * - `opts.expand=true`: フォルダを展開状態にしてから子を読む（新規作成した中身を見せる）。
 * - 展開済みフォルダ: 子を再取得してキャッシュ更新→再構築（追加/削除を反映）。
 * - 折りたたみ中かつ expand 指定なし: 表示に影響しないのでキャッシュ無効化のみ（次回展開で再取得）。
 * ルート直下の更新は本関数では扱わない（呼び出し側が listDir で treeEntries を取り直す）。
 */
export async function reloadTreeDir(
  dirPath: string,
  opts?: { expand?: boolean },
): Promise<void> {
  if (!treeState.fetch) return;
  const key = pathKey(dirPath);
  if (opts?.expand) treeState.expanded.add(key);
  if (!treeState.expanded.has(key)) {
    // 折りたたみ中＝見た目に出ないのでキャッシュを捨てるだけ（次回展開で新内容を取り直す）。
    treeState.childCache.delete(key);
    return;
  }
  try {
    const children = await treeState.fetch(dirPath);
    treeState.childCache.set(key, children);
  } catch {
    return; // 取得失敗は固めない（その行はそのまま）。
  }
  treeState.focusAfterRender = dirPath;
  rebuild();
}

/**
 * ツリーの展開状態と子キャッシュを破棄する（別フォルダを開く/切り替える時に呼ぶ）。
 * 前フォルダのパスが新フォルダに残って誤展開するのを防ぐ（複数フォルダ同時オープンはしない＝要件14章）。
 */
export function resetTreeExpansion(): void {
  treeState.expanded.clear();
  treeState.childCache.clear();
  treeState.focusAfterRender = undefined;
}

/**
 * 指定パス（フォルダ）とその配下の expanded/childCache エントリを掃除する（削除時・T5・指摘8）。
 * これをしないと、展開済みフォルダを削除→同じ親に同名フォルダを作り直したときに、旧 expanded が
 * 残って「最初から展開済み」かつ旧子（削除済みフォルダの中身）が幽霊表示される。
 * `path` 自身と `path + "/"` プレフィクスを持つ全キー（子孫）を expanded/childCache から消す。
 */
export function pruneTreeDir(path: string): void {
  const key = pathKey(path);
  const prefix = `${key}/`;
  for (const k of Array.from(treeState.expanded)) {
    if (k === key || k.startsWith(prefix)) treeState.expanded.delete(k);
  }
  for (const k of Array.from(treeState.childCache.keys())) {
    if (k === key || k.startsWith(prefix)) treeState.childCache.delete(k);
  }
}

/**
 * 現在の state（root＋展開集合＋子キャッシュ）からツリー DOM を**同期的に**組み直す。
 * 子の取得は非同期（toggleExpand）でキャッシュ確定後にこれを呼ぶため、ここでは I/O しない。
 */
function rebuild(): void {
  const ul = host();
  ul.replaceChildren();
  // ツリー自体はキーボードフォーカスを内部の treeitem へ委譲する（roving tabindex）。
  // host の <ul role="tree"> は tabindex を持たず、必ず treeitem の 1 つが tabIndex=0 になる。
  ul.removeAttribute("tabindex");
  for (const entry of treeState.rootEntries) {
    appendSubtree(ul, entry, 0);
  }
  // roving tabindex の初期化: 1 つだけ Tab 到達可能（0）にし、残りは -1。
  // 直前のキーボード展開/折りたたみで残したい行があればそこへ、無ければ先頭へ。
  initRovingTabindex(ul, treeState.focusAfterRender);
  treeState.focusAfterRender = undefined;
}

/** エントリ（＋展開済みなら子孫）を深さ depth で ul に追加する（同期・キャッシュ参照のみ）。 */
function appendSubtree(ul: HTMLElement, entry: TreeEntry, depth: number): void {
  const li = makeItem(entry, depth);
  ul.appendChild(li);
  if (entry.is_dir && treeState.expanded.has(pathKey(entry.path))) {
    const children = treeState.childCache.get(pathKey(entry.path)) ?? [];
    for (const child of children) {
      appendSubtree(ul, child, depth + 1);
    }
  }
}

function makeItem(entry: TreeEntry, depth: number): HTMLLIElement {
  const li = document.createElement("li");
  li.setAttribute("role", "treeitem");
  li.setAttribute("aria-selected", "false");
  li.dataset.path = entry.path;
  li.dataset.depth = String(depth);
  // 段階インデント（ui-design 7章 indent1=24px/indent2=42px 目安）。深さ可変なので CSS の calc で
  // base + depth * 段差を算出する（CSS 変数 --depth に深さを渡す）。
  li.style.setProperty("--depth", String(depth));
  const expanded = entry.is_dir && treeState.expanded.has(pathKey(entry.path));
  // ディレクトリは aria-expanded を**実値**で付与する（T2 must: 展開 true / 折りたたみ false）。
  if (entry.is_dir) {
    li.setAttribute("aria-expanded", expanded ? "true" : "false");
  }
  // roving tabindex: 既定は -1。rebuild 末尾で 1 つだけ 0 にする。
  li.tabIndex = -1;

  const mark = stateMark(entry, treeState.unread);

  // 開閉シェブロン（ui-mock .trow .twist 相当）。フォルダは ▾/▸、ファイルは同幅の空き（整列用）。
  // 装飾扱い（aria-hidden）。状態は aria-label / aria-expanded に集約する。
  const twist = document.createElement("span");
  twist.className = "tree-twist";
  twist.setAttribute("aria-hidden", "true");
  if (entry.is_dir) {
    twist.textContent = expanded ? "▾" : "▸";
    // シェブロン単独クリックでも開閉できる（行クリックと同経路だが伝播は止める）。
    twist.addEventListener("click", (e) => {
      e.stopPropagation();
      selectItem(li);
      void toggleExpand(entry);
    });
  }

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
  // ファイル名のみ（状態記号は別 span に分離して色を当てる＝ui-design 5章。タブの .tab-lead と同方針）。
  nameSpan.textContent = entry.name;
  if (mark.removed) {
    nameSpan.classList.add("removed"); // 削除済みは取り消し線（ui-design 5章）。span のみに限定。
  }
  li.append(twist, iconSpan, nameSpan);
  // 状態マーク（名前の右・ui-design 5/6章「左=種別アイコン、右=状態マーク」）。
  // 色は記号種別ごとにクラスで当てる: 差分あり ±=accent（青）／新規 ◆=diff-add（緑）。
  // 削除済みは取り消し線（記号なし）なので mark.suffix は空＝この span は出ない。
  // 色だけに依存しない（記号自体は残す・aria-label へも集約＝要件11.5）。
  if (mark.suffix && !mark.removed) {
    const markSpan = document.createElement("span");
    const isCreated = mark.suffix.includes(UNREAD_MARK.created);
    // 自身の新規 ◆=緑、自身/伝播の差分あり ±=青（伝播は CSS の data-unread=propagated で淡色化）。
    markSpan.className = isCreated ? "tree-mark new" : "tree-mark";
    markSpan.setAttribute("aria-hidden", "true"); // 状態は li の aria-label に集約する。
    markSpan.textContent = mark.suffix.trim();
    li.appendChild(markSpan);
  }
  applyAriaLabel(li, entry, mark);
  if (mark.propagated) {
    li.dataset.unread = "propagated"; // 伝播マーク（淡 ±）。視覚は CSS で淡色化。
  } else if (mark.suffix) {
    li.dataset.unread = "self";
  }

  // クリック: ファイルは開く・フォルダは開閉トグル（ファイルのように開かない＝要件4.1）。
  li.addEventListener("click", () => {
    selectItem(li);
    if (entry.is_dir) void toggleExpand(entry);
    else treeState.onOpen(entry);
  });
  // フォーカスを得たら roving tabindex を当該行へ移し選択を反映する（矢印移動の追従）。
  li.addEventListener("focus", () => selectItem(li));
  // キーボード操作（要件11.4/11.5: マウスなしで中心フロー完走＋WAI-ARIA tree の展開/折りたたみ）。
  li.addEventListener("keydown", (e) => onItemKeydown(e, li, entry));
  // 右クリック＝コンテキストメニュー（新規ファイル/新規フォルダ/削除＝要件11・T5）。
  // 既定のブラウザメニューは出さず（preventDefault）、対象行を選択してから呼び出し側へ座標を渡す。
  li.addEventListener("contextmenu", (e) => {
    if (!treeState.onContextMenu) return;
    e.preventDefault();
    e.stopPropagation();
    selectItem(li);
    li.focus();
    treeState.onContextMenu(entry, e.clientX, e.clientY);
  });
  return li;
}

/**
 * フォルダの展開/折りたたみをトグルする（遅延取得→キャッシュ→同期再構築）。
 * 子取得は非同期だが、取得完了後にキャッシュ確定→rebuild（同期）で展開状態を反映する。
 * 列挙失敗（権限/消失）でも UI は固めず、その行は展開しないまま残す（最上位「固まらない」）。
 */
async function toggleExpand(entry: TreeEntry): Promise<void> {
  if (!entry.is_dir) return;
  const key = pathKey(entry.path);
  if (treeState.expanded.has(key)) {
    // 折りたたみ: 集合から外して再構築（子 DOM が消える）。キャッシュは保持して再展開を速くする。
    treeState.expanded.delete(key);
    treeState.focusAfterRender = entry.path; // 折りたたんだフォルダ自身へフォーカスを残す。
    rebuild();
    return;
  }
  // 展開: 未キャッシュなら遅延取得してからキャッシュへ。fetch 未指定なら展開不可（直下のみ運用）。
  if (!treeState.childCache.has(key)) {
    if (!treeState.fetch) return;
    try {
      const children = await treeState.fetch(entry.path);
      treeState.childCache.set(key, children);
    } catch {
      // 取得失敗は展開しない（行はそのまま）。通知はフロント側方針に委ねここでは握り潰す。
      return;
    }
  }
  treeState.expanded.add(key);
  treeState.focusAfterRender = entry.path; // 展開したフォルダ自身へフォーカスを残す。
  rebuild();
}

// 拡張子→カテゴリ→アイコン symbol id のマッピング（ui-design 6章のカテゴリ集約表）。
// 未知拡張子は generic file（ic-file）へフォールバックする。フォルダは展開/折りたたみに
// かかわらず ic-folder（開閉の弁別はシェブロン ▾/▸ が担う）。
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

/** roving tabindex の初期化（指定パス or 先頭 treeitem のみ Tab 到達可能・残りは -1）。 */
function initRovingTabindex(ul: HTMLElement, focusPath?: string): void {
  const items = treeItems(ul);
  if (items.length === 0) return;
  let target = 0;
  if (focusPath) {
    const idx = items.findIndex((it) => it.dataset.path === focusPath);
    if (idx >= 0) target = idx;
  }
  // tabIndex は共通土台で 1 つだけ 0 に。aria-selected はツリー固有（focusPath 指定時のみ true）なので個別に当てる。
  applyRovingTabindex(items, target);
  items.forEach((it, i) => {
    it.setAttribute("aria-selected", i === target && focusPath ? "true" : "false");
  });
  // キーボード展開/折りたたみ直後はフォーカスも当該行へ戻す（操作位置を見失わない＝要件11.5）。
  if (focusPath && items[target]) {
    items[target].focus();
  }
}

/** ツリー内の treeitem 一覧（DOM 順＝展開された子も含む）。 */
function treeItems(ul: HTMLElement): HTMLElement[] {
  return collectByRole(ul, "treeitem");
}

/**
 * roving tabindex を当該行に移し選択（aria-selected）を反映する。
 * Tab で戻ってきたとき直前に操作した行へ戻れるよう、tabIndex=0 はツリー内に常に 1 つだけ保つ。
 */
function selectItem(li: HTMLElement): void {
  const items = treeItems(host());
  for (const node of items) node.setAttribute("aria-selected", "false");
  li.setAttribute("aria-selected", "true");
  applyRovingTabindex(items, li);
}

/**
 * treeitem 上のキー操作（要件11.4/11.5・WAI-ARIA tree）。
 * - ↑/↓: 表示中（展開済みの子を含む）の treeitem 間移動・Home/End: 先頭/末尾。
 * - Enter/Space: ファイルは開く・フォルダは開閉トグル。
 * - ArrowRight: 折りたたみフォルダは展開／展開済みフォルダは最初の子へ移動。
 * - ArrowLeft: 展開済みフォルダは折りたたむ／それ以外は親フォルダへ移動。
 */
function onItemKeydown(e: KeyboardEvent, li: HTMLElement, entry: TreeEntry): void {
  const ul = host();
  const items = treeItems(ul);
  const idx = items.indexOf(li);
  const expanded = entry.is_dir && treeState.expanded.has(pathKey(entry.path));
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
    case "ArrowRight": {
      e.preventDefault();
      if (entry.is_dir && !expanded) {
        // 折りたたみフォルダ → 展開（toggleExpand が rebuild 後に当該行へフォーカスを戻す）。
        void toggleExpand(entry);
      } else if (entry.is_dir && expanded) {
        // 展開済みフォルダ → 最初の子（DOM 順で次の行が深さ+1）へ移動。
        const next = items[idx + 1];
        if (next && depthOf(next) > depthOf(li)) focusItemAt(items, idx + 1);
      }
      break;
    }
    case "ArrowLeft": {
      e.preventDefault();
      if (entry.is_dir && expanded) {
        // 展開済みフォルダ → 折りたたむ。
        void toggleExpand(entry);
      } else {
        // ファイル or 折りたたみフォルダ → 親フォルダ（DOM をさかのぼり深さが 1 小さい直近行）へ移動。
        const parentIdx = parentIndexOf(items, idx);
        if (parentIdx >= 0) focusItemAt(items, parentIdx);
      }
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
      // Space/Enter: ファイルを開く（要件11.4「マウスなしで中心フロー完走」の起点）・フォルダは開閉。
      e.preventDefault();
      selectItem(li);
      if (entry.is_dir) void toggleExpand(entry);
      else treeState.onOpen(entry);
      break;
    }
  }
}

/** treeitem の深さ（data-depth）を取り出す。 */
function depthOf(li: HTMLElement): number {
  return Number(li.dataset.depth ?? "0");
}

/** 指定行の親フォルダ行の index（DOM をさかのぼり深さが 1 小さい直近行）。無ければ -1。 */
function parentIndexOf(items: HTMLElement[], idx: number): number {
  const myDepth = depthOf(items[idx]);
  if (myDepth === 0) return -1;
  for (let i = idx - 1; i >= 0; i--) {
    if (depthOf(items[i]) === myDepth - 1) return i;
  }
  return -1;
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
