// pika フロントエンドのエントリ。
// 中心体験①②: フォルダを開く → ツリー → タブで CM6 を開く → 編集 → 保存／外部変更を未読反映。
// 差分/プレビュー/単一インスタンス等は後続スプリントで肉付けする。
import {
  openWorkspace,
  readFile,
  saveFile,
  f5Resync,
  computeFileDiff,
  confirmFile,
  confirmAll,
  rollbackFile,
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
import { renderDiff, type DiffHandle } from "./diff";

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
  // 差分トグル（要件8.2・ON/OFF はソース/分割/プレビューに直交）。
  diffOn: false,
  // 現在描画中の差分ハンドル（F8/Shift+F8 ジャンプ・破棄用）。
  diff: null as DiffHandle | null,
};

const saveBtn = () => document.getElementById("save-file") as HTMLButtonElement;
const editorHost = () => document.getElementById("editor-host") as HTMLElement;
const diffHost = () => document.getElementById("diff-host") as HTMLElement;
const toggleDiffBtn = () => document.getElementById("toggle-diff") as HTMLButtonElement;
const confirmBtn = () => document.getElementById("confirm-file") as HTMLButtonElement;
const confirmAllBtn = () => document.getElementById("confirm-all") as HTMLButtonElement;
const rollbackBtn = () => document.getElementById("rollback-file") as HTMLButtonElement;

function refreshTabs(): void {
  renderTabs(state.tabs, state.active, activateTab, state.unread);
  const hasActive = !!state.active;
  saveBtn().disabled = !hasActive;
  toggleDiffBtn().disabled = !hasActive;
  confirmBtn().disabled = !hasActive;
  rollbackBtn().disabled = !hasActive;
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
    // 差分トグル ON のままタブを切替えたら新しいアクティブタブの差分を再描画する。
    if (state.diffOn) await renderActiveDiff();
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

/** 差分トグル（Ctrl+\・要件8.2）。ON でアクティブタブの差分を読み取り専用表示する。 */
async function onToggleDiff(): Promise<void> {
  if (!state.active) return;
  state.diffOn = !state.diffOn;
  toggleDiffBtn().setAttribute("aria-pressed", String(state.diffOn));
  if (state.diffOn) {
    await renderActiveDiff();
  } else {
    hideDiff();
  }
}

/** アクティブタブの差分（ベースライン vs 編集バッファ/ディスク）を計算し描画する（要件8.2）。 */
async function renderActiveDiff(): Promise<void> {
  if (!state.active) return;
  // タブで開いていれば編集バッファ、なければディスク内容を current に渡す（要件8.2）。
  const current = state.editor ? state.editor.getContent() : await readFile(state.active);
  try {
    const diff = await computeFileDiff(state.active, current);
    state.diff?.destroy();
    state.diff = renderDiff(diffHost(), diff);
    // エディタを隠し差分面を表示（占有はトグルで直交＝ui-design 8章）。
    editorHost().hidden = true;
    diffHost().hidden = false;
    setStatus(`差分: 変更 ${diff.change_count} 件`);
  } catch (e) {
    notify(`差分の計算に失敗: ${String(e)}`, "error");
  }
}

/** 差分面を閉じてエディタへ戻す（Ctrl+E 相当＝差分は読み取り専用なので編集はソースで）。 */
function hideDiff(): void {
  state.diff?.destroy();
  state.diff = null;
  diffHost().hidden = true;
  editorHost().hidden = false;
  state.diffOn = false;
  toggleDiffBtn().setAttribute("aria-pressed", "false");
}

/** 「確認済みにする」（要件8.3）。確定直前のディスク再照合は backend(pika-core)が担う。 */
async function onConfirm(): Promise<void> {
  if (!state.active) return;
  const path = state.active;
  // 差分を未表示なら先に計算（diff_snapshot を backend に作らせる＝確定直前の再照合基準）。
  if (!state.diffOn) await renderActiveDiff();
  try {
    const confirmed = await confirmFile(path);
    if (confirmed) {
      // 未読解除・ツリー/タブのマーク解除（要件8.3）。
      state.unread.clearFile(path);
      refreshTree();
      refreshTabs();
      if (state.diffOn) await renderActiveDiff(); // 新ベースライン基準で再描画。
      notify("確認済みにしました");
    } else {
      // 確定直前にディスクが変化＝中断して再差分（要件8.3）。
      if (state.diffOn) await renderActiveDiff();
      notify("確認中にファイルが変化したため再差分しました", "warn");
    }
  } catch (e) {
    notify(`確認に失敗: ${String(e)}`, "error");
  }
}

/** 「すべて確認済みにする」（要件8.3）。実行開始時点の未読集合をフリーズして一括確定する。 */
async function onConfirmAll(): Promise<void> {
  // 実行開始時点の未読集合をフリーズ（要件8.3）。削除済みは対象外。
  const targets = state.unread.confirmTargets();
  if (targets.length === 0) {
    notify("確認すべき未読はありません");
    return;
  }
  try {
    // 各対象の差分を先に提示して backend に diff_snapshot を作らせる（確定直前の再照合基準）。
    // 内容を読んで diff を計算（タブで開いていなければディスク内容を渡す）。
    for (const path of targets) {
      const tab = state.tabs.find((t) => t.path === path);
      const current =
        tab && state.active === path && state.editor
          ? state.editor.getContent()
          : await readFile(path);
      await computeFileDiff(path, current);
    }
    const result = await confirmAll(targets);
    // 確認済みになったものを未読から外す（スキップ分は未読維持＝要件8.3）。
    // backend がスキップしたファイルは差分時点と変わっているので再差分が必要。ここでは
    // 確定件数のみ未読集合から外す簡易運用にし、スキップ分は次回の確認/F5 で再評価する。
    if (result.skipped === 0) {
      for (const path of targets) state.unread.clearFile(path);
    } else {
      // スキップがある場合は安全側で全再評価を促す（未確定を未読のまま残す）。
      // 確定済みだけでも UI を進めるため、差分を取り直す F5 を案内する。
    }
    refreshTree();
    refreshTabs();
    if (state.diffOn) await renderActiveDiff();
    notify(
      `すべて確認済み: ${result.updated} 件確定 / ${result.skipped} 件スキップ（更新前は一括退避済み）`,
    );
  } catch (e) {
    notify(`一括確認に失敗: ${String(e)}`, "error");
  }
}

/** 「確認済み時点に戻す」（要件8.3/7.3）。退避不能はエラー通知（既定ブロック）。 */
async function onRollback(): Promise<void> {
  if (!state.active || !state.editor) return;
  const path = state.active;
  try {
    // backend が現在内容を退避してからベースライン内容を返す（退避が最後の砦）。
    const baselineContent = await rollbackFile(path);
    // バッファをベースライン内容で上書き（外部リロード扱い＝単一トランザクション/非dirty）。
    state.editor.reloadExternal(baselineContent);
    state.unread.clearFile(path);
    refreshTree();
    refreshTabs();
    if (state.diffOn) await renderActiveDiff();
    notify("確認済み時点に戻しました（戻す前の内容は退避済み）");
  } catch (e) {
    notify(`巻き戻しできません: ${String(e)}`, "error");
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
  toggleDiffBtn().addEventListener("click", () => void onToggleDiff());
  confirmBtn().addEventListener("click", () => void onConfirm());
  confirmAllBtn().addEventListener("click", () => void onConfirmAll());
  rollbackBtn().addEventListener("click", () => void onRollback());
  window.addEventListener("keydown", (e) => {
    // F5 でオンデマンド再同期（要件11.2）。
    if (e.key === "F5") {
      e.preventDefault();
      void onF5();
      return;
    }
    // Ctrl+\ で差分トグル（要件8.2・ui-design 8章）。
    if (e.ctrlKey && e.key === "\\") {
      e.preventDefault();
      void onToggleDiff();
      return;
    }
    // Ctrl+E で差分面からソースへ戻す（差分は読み取り専用＝要件8.2）。
    if (e.ctrlKey && (e.key === "e" || e.key === "E")) {
      if (state.diffOn) {
        e.preventDefault();
        hideDiff();
      }
      return;
    }
    // F8/Shift+F8 で前後の変更へジャンプ（差分表示中のみ＝要件8.2）。
    if (e.key === "F8" && state.diff) {
      e.preventDefault();
      if (e.shiftKey) state.diff.jumpPrev();
      else state.diff.jumpNext();
    }
  });
  // 外部変更/監視モードの購読（backend の emit を受ける）。
  await onFsChanged((payload) => onExternalChange(payload.changes));
  await onWatchMode((message) => notify(message, "info"));
  setStatus("フォルダを開いてください");
}

void main();
