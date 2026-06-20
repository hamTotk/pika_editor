// ファイルツリー（最薄ループ）。role=tree/treeitem の土台のみ。
// 種別アイコン・状態マーク（±/◆/取消線）・自然順・除外・伝播・F6 フォーカスは
// sprint 2/7（design doc 17章）で実装する。
import type { TreeEntry } from "../ipc";

const host = () => document.getElementById("tree") as HTMLElement;

/** ツリー直下のエントリを描画する。クリックでファイルを開く（onOpen 経由）。 */
export function renderTree(entries: TreeEntry[], onOpen: (entry: TreeEntry) => void): void {
  const ul = host();
  ul.replaceChildren();
  for (const entry of entries) {
    const li = document.createElement("li");
    li.setAttribute("role", "treeitem");
    li.setAttribute("aria-selected", "false");
    li.tabIndex = -1;
    // 状態マークのテキスト化は a11y スプリントで aria-label に集約する。
    li.textContent = `${entry.is_dir ? "📁" : "📄"} ${entry.name}`;
    if (!entry.is_dir) {
      li.addEventListener("click", () => {
        for (const node of ul.querySelectorAll('[role="treeitem"]')) {
          node.setAttribute("aria-selected", "false");
        }
        li.setAttribute("aria-selected", "true");
        onOpen(entry);
      });
    }
    ul.appendChild(li);
  }
}
