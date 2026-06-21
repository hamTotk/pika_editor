// タブ（要件5.3・ui-design 5章）。role=tablist/tab の土台＋未読/未保存/削除済みの重畳バッジ。
// 表示優先順位は 削除済み ＞ 未保存 ＞ 未読（要件5.3）とし、色だけに依存せず記号でも区別する。
// 全タブ一覧ドロップダウン・端のバッジは sprint 7（design doc 17章）で本実装する。
import { UNREAD_MARK, type UnreadStore } from "./unread";

const host = () => document.getElementById("tabs") as HTMLElement;

export interface TabModel {
  path: string;
  title: string;
}

/** 開いているタブを描画し、選択中にマークを付ける。未保存/未読/削除済みの重畳も反映する。 */
export function renderTabs(
  tabs: (TabModel & { dirty?: boolean })[],
  activePath: string | null,
  onActivate: (path: string) => void,
  unread?: UnreadStore,
): void {
  const el = host();
  el.replaceChildren();
  for (const tab of tabs) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.setAttribute("role", "tab");
    btn.setAttribute("aria-selected", String(tab.path === activePath));
    btn.dataset.path = tab.path;

    const badge = tabBadge(tab.path, tab.dirty ?? false, unread);
    btn.textContent = `${badge.prefix}${stripPrefix(tab.title)}${badge.suffix}`;
    if (badge.removed) {
      btn.style.textDecoration = "line-through"; // 削除済みは取り消し線（要件7.2）。
    }
    btn.addEventListener("click", () => onActivate(tab.path));
    el.appendChild(btn);
  }
}

/** タイトル先頭に付けた旧来の未保存印（● ）を一旦取り除く（重畳を一元計算するため）。 */
function stripPrefix(title: string): string {
  return title.replace(/^● /, "");
}

/**
 * 重畳バッジを計算する。優先順位 削除済み ＞ 未保存 ＞ 未読（要件5.3）。
 * - 削除済み: 取り消し線＋× 記号。
 * - 未保存: 先頭に ● 。
 * - 未読: 末尾に状態記号（± / ◆）。
 *
 * 削除済みと未保存/未読は別軸なので、削除済みでも未保存印は併記しうるが、
 * 「色だけに依存しない」原則で記号は常に併用する（要件11.5）。
 */
function tabBadge(
  path: string,
  dirty: boolean,
  unread?: UnreadStore,
): { prefix: string; suffix: string; removed: boolean } {
  const kind = unread?.get(path);
  const removed = kind === "removed";
  const prefix = dirty ? "● " : "";
  let suffix = "";
  if (removed) {
    suffix = ` ${UNREAD_MARK.removed}`;
  } else if (kind) {
    suffix = ` ${UNREAD_MARK[kind]}`;
  }
  return { prefix, suffix, removed };
}
