// ツリーの右クリック操作（新規作成/削除）とコンテキストメニュー（S8・main.ts から抽出）。
//
// 新規ファイル/フォルダの作成（要件11）・削除（ごみ箱へ移動・要件11）・作成/削除後のツリー階層更新を担う。
// 確認フローは自前モーダル（confirmModal/promptText）で行う（window.confirm はこの Tauri/WebView2 ビルドで
// ダイアログを出さず即 true を返すため使わない）。挙動は main.ts 在籍時から一切変えていない（純粋リファクタ）。
// state とコールバック（refreshTree/openFile/markSelfCreated）は ctx で注入する。

import type { AppShellState } from "../app/types";
import type { TreeEntry } from "../ipc";
import { listDir, createEntry, deleteEntry } from "../ipc";
import { reloadTreeDir, pruneTreeDir } from "./tree";
import { promptText, confirmModal } from "./modal";
import { openContextMenu } from "./context-menu";
import { notify } from "./notifications";
import { parentDir, samePath } from "../util/path";

/** ツリー操作の依存（state 参照＋main.ts のコールバック）。 */
export interface TreeActionsContext {
  /** メインWebView シェルの可変状態（folder/treeEntries/editor を読み書きする）。 */
  state: AppShellState;
  /** 現在の treeEntries をツリーへ再描画する（main.ts の refreshTree）。 */
  refreshTree(): void;
  /** ファイルを開く（作成直後の自動オープン・main.ts の openFile）。 */
  openFile(entry: TreeEntry): Promise<void>;
  /** 自作成パスを watcher の created 未読抑制へ登録する（main.ts の selfCreatedPaths へ記録）。 */
  markSelfCreated(path: string): void;
}

/** ツリー操作ハンドル（main へ公開するのはコンテキストメニュー 2 種のみ）。 */
export interface TreeActions {
  /** ツリー項目の右クリックメニュー（ファイル/フォルダ＝要件11）。 */
  showTreeContextMenu(entry: TreeEntry, x: number, y: number): void;
  /** ツリーの空き領域（ルート）右クリックメニュー（新規作成のみ）。 */
  showRootContextMenu(x: number, y: number): void;
}

/** ツリー操作（作成/削除/コンテキストメニュー）を組み立てる。挙動は main.ts 在籍時と不変。 */
export function createTreeActions(ctx: TreeActionsContext): TreeActions {
  const state = ctx.state;

  /**
   * 新規ファイル/フォルダを作成する（要件11）。名前を尋ね、backend で作成し、ツリーを更新する。
   * ファイルは作成後に開く（中心体験へ即接続）。フォルダは作成先を展開して中身を見せる。
   */
  async function onCreateEntry(dir: string, isDir: boolean, expandDir: boolean): Promise<void> {
    const what = isDir ? "フォルダ" : "ファイル";
    const name = await promptText(
      `新規${what}の名前を入力してください`,
      isDir ? "新しいフォルダ" : "新しいファイル.md",
    );
    if (name === null) return; // キャンセル。
    const trimmed = name.trim();
    if (!trimmed) {
      notify("名前を入力してください", "warn");
      return;
    }
    try {
      const created = await createEntry(dir, trimmed, isDir);
      // 自作成は watcher の created イベントを未読(◆)にしない（指摘4）。イベント到着時に消費する。
      ctx.markSelfCreated(created);
      await refreshTreeDir(dir, { expand: expandDir });
      if (!isDir) {
        // 作成した空ファイルを開く（テキスト/非テキスト判定は openFile が拡張子で行う）。
        await ctx.openFile({ name: trimmed, path: created, is_dir: false });
        // 作成直後にすぐ編集を始められるようエディタへフォーカスを移す（指摘9）。
        // refreshTreeDir/openFile の途中でツリー行へフォーカスが移っているため明示的に取り戻す。
        state.editor?.focusEditor();
      }
      notify(`${what}を作成しました: ${trimmed}`);
    } catch (e) {
      notify(`${what}の作成に失敗しました: ${String(e)}`, "error");
    }
  }

  /** 削除（ごみ箱へ移動・要件11）。確認のうえ backend でごみ箱へ送り、ツリーを更新する。 */
  async function onDeleteEntry(entry: TreeEntry): Promise<void> {
    const what = entry.is_dir ? "フォルダ" : "ファイル";
    const ok = await confirmModal(
      `${what}「${entry.name}」をごみ箱へ移動します。よろしいですか？\n` +
        `（完全削除ではなく OS のごみ箱へ移動するので、必要なら復元できます）`,
      { okLabel: "ごみ箱へ移動", danger: true },
    );
    if (!ok) return;
    try {
      await deleteEntry(entry.path);
      // 削除したフォルダ自身とその配下の展開状態/子キャッシュを掃除する（同名再作成時の幽霊表示防止・指摘8）。
      if (entry.is_dir) pruneTreeDir(entry.path);
      await refreshTreeDir(parentDir(entry.path));
      notify(`ごみ箱へ移動しました: ${entry.name}`);
      // 開いているタブの追従（「削除済み」表示）は watcher の removed イベント（onExternalChange）が担う。
    } catch (e) {
      notify(`削除に失敗しました: ${String(e)}`, "error");
    }
  }

  /** 作成/削除後にツリーの該当階層を更新する（ルートは listDir で取り直し、サブフォルダは reloadTreeDir）。 */
  async function refreshTreeDir(dir: string, opts?: { expand?: boolean }): Promise<void> {
    if (state.folder && samePath(dir, state.folder)) {
      try {
        const entries = await listDir(state.folder);
        state.treeEntries = entries;
        ctx.refreshTree();
      } catch {
        // 取得失敗は固めない（現状維持）。
      }
      return;
    }
    await reloadTreeDir(dir, opts);
  }

  /** ツリー項目の右クリックメニュー（ファイル/フォルダ＝要件11）。 */
  function showTreeContextMenu(entry: TreeEntry, x: number, y: number): void {
    // 新規作成先: フォルダ上ならその中、ファイル上ならその親フォルダ。
    const targetDir = entry.is_dir ? entry.path : parentDir(entry.path);
    const expandAfter = entry.is_dir; // フォルダ内に作ったら展開して中身を見せる。
    openContextMenu(
      [
        { label: "新規ファイル", run: () => void onCreateEntry(targetDir, false, expandAfter) },
        { label: "新規フォルダ", run: () => void onCreateEntry(targetDir, true, expandAfter) },
        "sep",
        { label: "削除（ごみ箱へ）", danger: true, run: () => void onDeleteEntry(entry) },
      ],
      x,
      y,
    );
  }

  /** ツリーの空き領域（ルート）右クリックメニュー（新規作成のみ）。 */
  function showRootContextMenu(x: number, y: number): void {
    if (!state.folder) return;
    const dir = state.folder;
    openContextMenu(
      [
        { label: "新規ファイル", run: () => void onCreateEntry(dir, false, false) },
        { label: "新規フォルダ", run: () => void onCreateEntry(dir, true, false) },
      ],
      x,
      y,
    );
  }

  return { showTreeContextMenu, showRootContextMenu };
}
