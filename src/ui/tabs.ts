// タブ（最薄ループ）。role=tablist/tab の土台のみ。
// 未読/未保存/削除済みの重畳バッジ・優先順位・全タブドロップダウンは
// sprint 3/7（design doc 5章・17章）で実装する。
const host = () => document.getElementById("tabs") as HTMLElement;

export interface TabModel {
  path: string;
  title: string;
}

/** 開いているタブを描画し、選択中にマークを付ける。 */
export function renderTabs(
  tabs: TabModel[],
  activePath: string | null,
  onActivate: (path: string) => void,
): void {
  const el = host();
  el.replaceChildren();
  for (const tab of tabs) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.setAttribute("role", "tab");
    btn.setAttribute("aria-selected", String(tab.path === activePath));
    btn.textContent = tab.title;
    btn.addEventListener("click", () => onActivate(tab.path));
    el.appendChild(btn);
  }
}
