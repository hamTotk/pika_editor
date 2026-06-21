// ファイルツリー（要件4.1/4.2・ui-design 5/6章）。role=tree/treeitem の土台＋未読状態マーク。
// 種別アイコン詳細・自然順・除外・F6 フォーカスは sprint 7（design doc 17章）で本実装する。
// 本スプリント（sprint 2）では外部変更（fs-changed）由来の未読マーク（± 変更 / ◆ 新規 / 取消線 削除）と
// フォルダ伝播（淡い ±）を反映する配線を入れる。
import type { TreeEntry } from "../ipc";
import { UNREAD_MARK, type UnreadStore } from "./unread";

const host = () => document.getElementById("tree") as HTMLElement;

/** ツリー直下のエントリを描画する。クリックでファイルを開く（onOpen 経由）。 */
export function renderTree(
  entries: TreeEntry[],
  onOpen: (entry: TreeEntry) => void,
  unread?: UnreadStore,
): void {
  const ul = host();
  ul.replaceChildren();
  for (const entry of entries) {
    ul.appendChild(makeItem(entry, onOpen, unread));
  }
}

function makeItem(
  entry: TreeEntry,
  onOpen: (entry: TreeEntry) => void,
  unread?: UnreadStore,
): HTMLLIElement {
  const li = document.createElement("li");
  li.setAttribute("role", "treeitem");
  li.setAttribute("aria-selected", "false");
  li.tabIndex = -1;
  li.dataset.path = entry.path;

  const icon = entry.is_dir ? "📁" : "📄";
  const mark = stateMark(entry, unread);
  // 状態マークは色だけに依存しない記号（要件11.5）。aria-label への集約は sprint 7。
  li.textContent = `${icon} ${entry.name}${mark.suffix}`;
  if (mark.removed) {
    li.style.textDecoration = "line-through"; // 削除済みは取り消し線（ui-design 5章）。
  }
  if (mark.propagated) {
    li.dataset.unread = "propagated"; // 伝播マーク（淡 ±）。視覚は CSS で淡色化。
  } else if (mark.suffix) {
    li.dataset.unread = "self";
  }

  if (!entry.is_dir) {
    li.addEventListener("click", () => {
      for (const node of host().querySelectorAll('[role="treeitem"]')) {
        node.setAttribute("aria-selected", "false");
      }
      li.setAttribute("aria-selected", "true");
      onOpen(entry);
    });
  }
  return li;
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
