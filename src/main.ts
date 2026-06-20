// pika フロントエンドのエントリ（最薄ループの貫通）。
// 中心体験① の縦切り: フォルダを開く → ツリー → タブで CM6 を開く → 編集 → 保存。
// 監視/差分/プレビュー/単一インスタンス等は後続スプリントで肉付けする。
import { openWorkspace, readFile, saveFile, type TreeEntry } from "./ipc";
import { initTheme } from "./theme";
import { renderTree } from "./ui/tree";
import { renderTabs, type TabModel } from "./ui/tabs";
import { notify } from "./ui/notifications";
import { setStatus } from "./ui/status";
import { createEditor, type EditorHandle } from "./editor";

interface OpenTab extends TabModel {
  dirty: boolean;
}

const state = {
  tabs: [] as OpenTab[],
  active: null as string | null,
  editor: null as EditorHandle | null,
};

const saveBtn = () => document.getElementById("save-file") as HTMLButtonElement;
const editorHost = () => document.getElementById("editor-host") as HTMLElement;

function refreshTabs(): void {
  renderTabs(state.tabs, state.active, activateTab);
  saveBtn().disabled = !state.active;
}

async function activateTab(path: string): Promise<void> {
  state.active = path;
  try {
    const content = await readFile(path);
    state.editor?.destroy();
    state.editor = createEditor(editorHost(), content, () => markDirty(path));
    refreshTabs();
  } catch (e) {
    notify(`開けませんでした: ${String(e)}`, "error");
  }
}

function markDirty(path: string): void {
  const tab = state.tabs.find((t) => t.path === path);
  if (tab && !tab.dirty) {
    tab.dirty = true;
    tab.title = `● ${tab.title.replace(/^● /, "")}`;
    refreshTabs();
  }
}

async function openFile(entry: TreeEntry): Promise<void> {
  if (!state.tabs.some((t) => t.path === entry.path)) {
    state.tabs.push({ path: entry.path, title: entry.name, dirty: false });
  }
  await activateTab(entry.path);
}

async function onOpenFolder(): Promise<void> {
  // 最薄ループではパス入力でフォルダを開く（ネイティブ選択ダイアログ=dialog plugin は
  // capability を増やすため後続スプリントで導入を見極める。design doc 9章 最小権限）。
  const input = document.getElementById("folder-path") as HTMLInputElement;
  const dir = input.value.trim();
  if (!dir) {
    notify("フォルダのパスを入力してください", "warn");
    return;
  }
  try {
    const entries = await openWorkspace(dir);
    renderTree(entries, (entry) => void openFile(entry));
    setStatus(`${dir}（${entries.length} 件）`);
  } catch (e) {
    notify(`フォルダを開けませんでした: ${String(e)}`, "error");
  }
}

async function onSave(): Promise<void> {
  if (!state.active || !state.editor) return;
  try {
    await saveFile(state.active, state.editor.getContent());
    const tab = state.tabs.find((t) => t.path === state.active);
    if (tab) {
      tab.dirty = false;
      tab.title = tab.title.replace(/^● /, "");
    }
    refreshTabs();
    notify("保存しました");
  } catch (e) {
    notify(`保存に失敗しました: ${String(e)}`, "error");
  }
}

function main(): void {
  initTheme();
  document.getElementById("open-folder")?.addEventListener("click", () => void onOpenFolder());
  saveBtn().addEventListener("click", () => void onSave());
  setStatus("フォルダを開いてください");
}

main();
