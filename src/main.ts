// pika フロントエンドのエントリ。
// 中心体験①②: フォルダを開く → ツリー → タブで CM6 を開く → 編集 → 保存／外部変更を未読反映。
// 差分/プレビュー/単一インスタンス等は後続スプリントで肉付けする。
import {
  openWorkspace,
  readFile,
  saveFile,
  f5Resync,
  onFsChanged,
  onWatchMode,
  type TreeEntry,
} from "./ipc";
import { initTheme } from "./theme";
import { renderTree } from "./ui/tree";
import { renderTabs, type TabModel } from "./ui/tabs";
import { notify } from "./ui/notifications";
import { setStatus } from "./ui/status";
import { UnreadStore } from "./ui/unread";
import { createEditor, type EditorHandle } from "./editor";

interface OpenTab extends TabModel {
  dirty: boolean;
}

const state = {
  tabs: [] as OpenTab[],
  active: null as string | null,
  editor: null as EditorHandle | null,
  // 直近に描画したツリー直下エントリ（外部変更で未読マークを再描画するため保持）。
  treeEntries: [] as TreeEntry[],
  // 外部変更由来の未読状態（要件4.2）。ツリー/タブで共有する単一源。
  unread: new UnreadStore(),
  // 現在開いているフォルダ（ステータス再表示用）。
  folder: null as string | null,
};

const saveBtn = () => document.getElementById("save-file") as HTMLButtonElement;
const editorHost = () => document.getElementById("editor-host") as HTMLElement;

function refreshTabs(): void {
  renderTabs(state.tabs, state.active, activateTab, state.unread);
  saveBtn().disabled = !state.active;
}

function refreshTree(): void {
  renderTree(state.treeEntries, (entry) => void openFile(entry), state.unread);
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
  // 最薄ループではパス入力でフォルダを開く（ネイティブ選択ダイアログは capability を増やすため後続）。
  const input = document.getElementById("folder-path") as HTMLInputElement;
  const dir = input.value.trim();
  if (!dir) {
    notify("フォルダのパスを入力してください", "warn");
    return;
  }
  try {
    const entries = await openWorkspace(dir);
    state.folder = dir;
    state.treeEntries = entries;
    // 初回オープンは全既読スタート（要件8.1）。未読は外部変更（fs-changed）で付く。
    state.unread = new UnreadStore();
    refreshTree();
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
    }
    // 自身の保存では未読を付けない（backend のハッシュ一致抑制と二重で担保）。
    state.unread.clearFile(state.active);
    refreshTabs();
    refreshTree();
    notify("保存しました");
  } catch (e) {
    notify(`保存に失敗しました: ${String(e)}`, "error");
  }
}

/** 外部変更（fs-changed）を未読ストアへ適用し、ツリー/タブを再描画する（要件4.2/7.2）。 */
function onExternalChange(changes: import("./ipc").FsChange[]): void {
  if (changes.length === 0) return;
  state.unread.apply(changes);
  // 開いているクリーン（未保存変更なし）タブへの外部変更は自動リロード（要件7.2）。
  // 未保存変更があるタブは自動リロードせず通知のみ（衝突処理は sprint 3 で本実装）。
  void autoReloadCleanTabs(changes);
  refreshTree();
  refreshTabs();
  // ステータスに件数を出す（aria-label 化は sprint 7）。
  if (state.folder) {
    setStatus(`${state.folder}（未読 ${state.unread.unreadCount()} 件）`);
  }
}

/** クリーンな現在タブが外部変更されたら CM6 へ単一トランザクションで反映する（要件7.2/5.1）。 */
async function autoReloadCleanTabs(changes: import("./ipc").FsChange[]): Promise<void> {
  if (!state.active || !state.editor) return;
  const activeTab = state.tabs.find((t) => t.path === state.active);
  if (!activeTab || activeTab.dirty) return; // 未保存変更があれば自動リロードしない。
  const hit = changes.find((c) => c.kind === "modified" && c.path === state.active);
  if (!hit) return;
  try {
    const content = await readFile(state.active);
    state.editor.reloadExternal(content);
    // 自動リロードで現在タブを見たので未読は解除（外部変更を反映済み）。
    state.unread.clearFile(state.active);
    refreshTabs();
    refreshTree();
  } catch (e) {
    notify(`外部変更の反映に失敗: ${String(e)}`, "warn");
  }
}

/** F5 = オンデマンド再同期（要件7.1/11.2）。監視不能 FS でも未読を取りこぼさない。 */
async function onF5(): Promise<void> {
  try {
    const count = await f5Resync();
    notify(count > 0 ? `再同期: ${count} 件の変更を検知` : "再同期: 変更なし");
  } catch (e) {
    notify(`再同期に失敗: ${String(e)}`, "error");
  }
}

async function main(): Promise<void> {
  initTheme();
  document.getElementById("open-folder")?.addEventListener("click", () => void onOpenFolder());
  saveBtn().addEventListener("click", () => void onSave());
  // F5 でオンデマンド再同期（要件11.2）。
  window.addEventListener("keydown", (e) => {
    if (e.key === "F5") {
      e.preventDefault();
      void onF5();
    }
  });
  // 外部変更/監視モードの購読（backend の emit を受ける）。
  await onFsChanged((payload) => onExternalChange(payload.changes));
  await onWatchMode((message) => notify(message, "info"));
  setStatus("フォルダを開いてください");
}

void main();
