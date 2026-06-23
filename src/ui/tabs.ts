// タブ（要件5.3・ui-design 5章）。role=tablist/tab の土台＋未読/未保存/削除済みの重畳バッジ。
// 表示優先順位は 削除済み ＞ 未保存 ＞ 差分あり（要件5.3）とし、色だけに依存せず記号でも区別する。
// 状態記号の配置は ui-mock 準拠（T4・C2/C3）:
//   - 差分あり ±／新規 ◆ … タイトルの**左**（lead span・accent / diff-add 緑）
//   - 未保存 ● … **閉じる位置**（x span を ● で表示・accent。ホバーで ✕ へ）
//   - 削除済み … タイトルの取り消し線（記号なし・F-028 でタイトル span のみに限定）
// sprint 7（design doc 17章）で **キーボード操作性**を本実装する: roving tabindex（tablist 内で常に
// 1 つだけ tabIndex=0）＋ ←/→ でタブ移動・Home/End で先頭/末尾（WAI-ARIA tablist パターン）。
// バッジの状態は aria-label にテキスト化して読み上げ到達を確実にする（要件11.5）。
import { UNREAD_MARK, type UnreadStore } from "./unread";

const host = () => document.getElementById("tabs") as HTMLElement;

export interface TabModel {
  path: string;
  title: string;
}

/** 開いているタブを描画し、選択中にマークを付ける。未保存/差分あり/削除済みの重畳も反映する。 */
export function renderTabs(
  tabs: (TabModel & { dirty?: boolean })[],
  activePath: string | null,
  onActivate: (path: string) => void,
  unread?: UnreadStore,
  onClose?: (path: string) => void,
): void {
  const el = host();
  el.replaceChildren();
  for (const tab of tabs) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.setAttribute("role", "tab");
    const selected = tab.path === activePath;
    btn.setAttribute("aria-selected", String(selected));
    // roving tabindex: 選択中タブのみ Tab 到達可能（0）・残りは -1（WAI-ARIA tablist）。
    btn.tabIndex = selected ? 0 : -1;
    btn.dataset.path = tab.path;

    const badge = tabBadge(tab.path, tab.dirty ?? false, unread);
    const title = stripPrefix(tab.title);

    // ラベルを部品ごとに分ける（F-028: 取り消し線をタイトルだけに限定し、未保存印 ● や lead 記号は
    // 装飾対象から外す）。色のみに依存しない記号表現（要件11.5）は span 構成でもそのまま維持する。

    // lead（タイトルの左）＝差分あり ±／新規 ◆（ui-mock .tab .lead / .lead.new）。削除済みは
    // 取り消し線で示すので lead 記号は出さない（重畳優先 削除済み ＞ 差分あり）。
    if (badge.lead) {
      const leadSpan = document.createElement("span");
      leadSpan.className = badge.lead === UNREAD_MARK.created ? "tab-lead new" : "tab-lead";
      leadSpan.setAttribute("aria-hidden", "true"); // 状態は aria-label に集約する。
      leadSpan.textContent = badge.lead;
      btn.appendChild(leadSpan);
    }

    const titleSpan = document.createElement("span");
    titleSpan.className = "tab-title";
    titleSpan.textContent = title;
    if (badge.removed) {
      titleSpan.classList.add("removed"); // 削除済みは取り消し線（要件7.2）。タイトル span のみに限定。
    }
    btn.appendChild(titleSpan);

    // 閉じる位置（ui-mock .tab .x）。通常は ✕、未保存(dirty)時は ● を出す（CSS の .dirty で）。
    // ● はホバーで ✕ へ変わる（CSS）。クリックでそのタブを閉じる（onActivate に伝播させない）。
    const closeSpan = document.createElement("span");
    closeSpan.className = badge.dirty ? "tab-x dirty" : "tab-x";
    closeSpan.setAttribute("role", "button");
    closeSpan.setAttribute("aria-label", "閉じる");
    closeSpan.tabIndex = -1; // タブ本体の roving tabindex を乱さない（Tab では止まらず、クリック/Enter で操作）。
    if (onClose) {
      closeSpan.addEventListener("click", (e) => {
        e.stopPropagation(); // × クリックはタブ活性化（onActivate）へ伝播させない。
        onClose(tab.path);
      });
      // キーボードでも閉じられるよう Enter/Space を受ける（role=button の慣例）。
      closeSpan.addEventListener("keydown", (e) => {
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault();
          e.stopPropagation();
          onClose(tab.path);
        }
      });
    }
    btn.appendChild(closeSpan);

    // 状態（未保存/差分あり/削除済み）を aria-label にテキスト化して読み上げ到達を確実にする（要件11.5）。
    btn.setAttribute("aria-label", tabAriaLabel(title, tab.dirty ?? false, badge));
    btn.addEventListener("click", () => onActivate(tab.path));
    btn.addEventListener("keydown", (e) => onTabKeydown(e, btn, onActivate));
    el.appendChild(btn);
  }
}

/** タブ間のキーボード移動（←/→・Home/End＝WAI-ARIA tablist パターン・要件11.5）。 */
function onTabKeydown(
  e: KeyboardEvent,
  btn: HTMLButtonElement,
  onActivate: (path: string) => void,
): void {
  const tabs = Array.from(host().querySelectorAll<HTMLButtonElement>('[role="tab"]'));
  const idx = tabs.indexOf(btn);
  let target: HTMLButtonElement | undefined;
  switch (e.key) {
    case "ArrowRight":
      target = tabs[(idx + 1) % tabs.length];
      break;
    case "ArrowLeft":
      target = tabs[(idx - 1 + tabs.length) % tabs.length];
      break;
    case "Home":
      target = tabs[0];
      break;
    case "End":
      target = tabs[tabs.length - 1];
      break;
    default:
      return;
  }
  if (!target) return;
  e.preventDefault();
  // 移動先のタブへフォーカス＋アクティブ化（tablist は自動アクティベーション）。
  target.focus();
  const path = target.dataset.path;
  if (path) onActivate(path);
}

/** タブの状態を読み上げ用テキストに集約する（色/記号に依存しない・要件11.5）。 */
function tabAriaLabel(
  title: string,
  dirty: boolean,
  badge: { removed: boolean; lead: string },
): string {
  const states: string[] = [];
  if (dirty) states.push("未保存");
  if (badge.removed) states.push("削除済み");
  else if (badge.lead === UNREAD_MARK.created) states.push("新規");
  else if (badge.lead === UNREAD_MARK.modified) states.push("差分あり");
  return states.length > 0 ? `${title}（${states.join("・")}）` : title;
}

/** タイトル先頭に付けた旧来の未保存印（● ）を一旦取り除く（重畳を一元計算するため）。 */
function stripPrefix(title: string): string {
  return title.replace(/^● /, "");
}

/**
 * 重畳バッジを計算する。優先順位 削除済み ＞ 未保存 ＞ 差分あり（要件5.3）。
 * - 削除済み: 取り消し線（lead 記号は出さない）。
 * - 未保存: 閉じる位置の ●（dirty フラグ）。
 * - 差分あり/新規: タイトル左の lead 記号（± / ◆）。
 *
 * 削除済みと未保存は別軸なので、削除済みでも未保存（●）は併記しうる。
 * 「色だけに依存しない」原則で記号は常に併用する（要件11.5）。
 */
function tabBadge(
  path: string,
  dirty: boolean,
  unread?: UnreadStore,
): { lead: string; dirty: boolean; removed: boolean } {
  const kind = unread?.get(path);
  const removed = kind === "removed";
  // lead は差分あり ±／新規 ◆ のみ（削除済みは取り消し線で表すので lead は空）。
  let lead = "";
  if (!removed && (kind === "modified" || kind === "created")) {
    lead = UNREAD_MARK[kind];
  }
  return { lead, dirty, removed };
}
