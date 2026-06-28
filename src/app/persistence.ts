// アプリ状態（state.json）の収集・保存・復元（S8・main.ts から抽出）。
//
// **最上位原則「データを失わない」**: safeEmpty（未知バージョン/破損で空起動）の間は保存を控え、読めない
// state.json を上書きしない。保存はデバウンス合体し、終了時（beforeunload）は flush で確実に書く。復元は
// version 安全側・3分岐（ワークスペース消失=空状態/タブ消失=削除済み表示/別物=未読復元）を backend(pika-core)
// が判定し、ここはその status を受けて UI へ反映する。収集内容・デバウンス・最新性ガード・復元順序・
// restoreTabPosition のタイミングは main.ts 在籍時から一切変えていない（純粋リファクタ）。
// state と各ハンドラ（captureActivePosition/refreshTree/refreshTabs/updateTreeHeader/newTab/openFile/
// activateTab）は ctx（参照＋コールバック）で注入する。

import type { AppShellState, OpenTab } from "./types";
import type { AppState, TreeEntry } from "../ipc";
import { saveAppState, restoreAppState, openWorkspace } from "../ipc";
import { notify } from "../ui/notifications";
import { resetTreeExpansion } from "../ui/tree";
import { UnreadStore } from "../ui/unread";
import { setStatus } from "../ui/status";
import { basename } from "../util/path";

/** 永続化の依存（state 参照＋main.ts のコールバック）。 */
export interface PersistenceContext {
  /** メインWebView シェルの可変状態（収集/復元で読み書きする）。 */
  state: AppShellState;
  /** アクティブタブのカーソル/スクロール（と dirty 時の編集中テキスト）を現在タブへ写す。 */
  captureActivePosition(): void;
  /** 現在の treeEntries をツリーへ再描画する。 */
  refreshTree(): void;
  /** タブ列を再描画する。 */
  refreshTabs(): void;
  /** ツリーヘッダ（エクスプローラー — <フォルダ名>）を更新する。 */
  updateTreeHeader(): void;
  /** 既定値で OpenTab を作る。 */
  newTab(path: string, title: string): OpenTab;
  /** ファイルを開く（復元時のタブ展開）。 */
  openFile(entry: TreeEntry): Promise<void>;
  /** 指定パスのタブをアクティブ化する。 */
  activateTab(path: string): Promise<void>;
}

/** 永続化ハンドル（main へ公開する関数群）。 */
export interface Persistence {
  /** アプリ状態をデバウンス保存する（連続操作の書込合体）。 */
  persistAppState(): void;
  /** デバウンスせず即座に state.json を保存する（終了時 flush 用）。 */
  persistAppStateNow(): Promise<void>;
  /** 起動時に state.json を復元する。 */
  restoreOnStartup(): Promise<void>;
  /** 終了直前にデバウンス保留を破棄する（flush と二重に書かないため）。beforeunload で呼ぶ。 */
  cancelPendingPersist(): void;
}

/** 永続化（収集/保存/復元）を組み立てる。挙動は main.ts 在籍時と不変。 */
export function createPersistence(ctx: PersistenceContext): Persistence {
  const state = ctx.state;

  /** 現在の UI 状態を AppState へ写す（state.json 保存用・要件10.1）。 */
  function collectAppState(): AppState {
    // アクティブタブのカーソル/スクロールを最新化してから収集する（実値で保存・eval high）。
    ctx.captureActivePosition();
    const activeIdx = state.tabs.findIndex((t) => t.path === state.active);
    return {
      version: 1,
      workspace: state.folder ?? undefined,
      // 各タブの実カーソル/スクロール/content_hash を保存する（ダミー値固定の解消・eval high）。
      // diff_on/view_mode はアプリ全体で 1 つ（ソース/プレビューと差分トグルは現状グローバル）なので
      // 現在の表示状態を全タブに反映する。content_hash は開く/保存/外部リロード時に実値で埋めている。
      tabs: state.tabs.map((t) => ({
        path: t.path,
        cursor_line: t.cursorLine,
        cursor_column: t.cursorColumn,
        scroll_top: t.scrollTop,
        view_mode: state.viewMode,
        diff_on: state.diffOn,
        content_hash: t.contentHash,
      })),
      active_tab: activeIdx < 0 ? 0 : activeIdx,
      expanded_dirs: [],
      window: { x: 0, y: 0, width: 0, height: 0, maximized: false },
      // recent は backend(note_recent)が read-modify-write で別管理するため空で送る
      // （save_app_state が recent を上書きしないよう backend 側で保つ。下記コメント参照）。
      recent: { files: [], folders: [] },
    };
  }

  /** persistAppState のデバウンスタイマー（連続操作で書込が積み重なるのを抑える・eval medium）。 */
  let persistTimer: number | null = null;

  /**
   * アプリ状態をアトミック保存する（要件10.1）。未知バージョン/破損で空起動した（safeEmpty）間は
   * 保存しない＝読めない state.json を上書きしない（最上位原則「データを失わない」）。
   *
   * 連続でタブ/フォルダを開く操作で都度アトミック書込（同期 FS I/O）が積み重なるため、
   * 短時間デバウンス（合体）する（eval medium）。終了時（beforeunload）は flush で確実に書く。
   */
  function persistAppState(): void {
    if (state.safeEmpty) return;
    if (persistTimer !== null) window.clearTimeout(persistTimer);
    persistTimer = window.setTimeout(() => {
      persistTimer = null;
      void persistAppStateNow();
    }, 400);
  }

  /** デバウンスせず即座に state.json を保存する（終了時 flush 用）。 */
  async function persistAppStateNow(): Promise<void> {
    if (state.safeEmpty) return;
    try {
      await saveAppState(collectAppState());
    } catch (e) {
      // 保存失敗は通知のみ（状態保存はベストエフォート・データ本体は失わない）。
      notify(`状態の保存に失敗: ${String(e)}`, "warn");
    }
  }

  /**
   * 終了直前にデバウンス保留を破棄する（eval medium #30）。クリアしないと、終了直前の即時 flush 後に
   * デバウンス分が後から発火し、二重保存や（終了で UI が消えた後の）未完走で終了直前保存が取りこぼされうる。
   */
  function cancelPendingPersist(): void {
    if (persistTimer !== null) {
      window.clearTimeout(persistTimer);
      persistTimer = null;
    }
  }

  /**
   * 起動時に state.json を復元する（要件10.1/13）。version 安全側・復元3分岐の判定は backend(pika-core)。
   * ワークスペース消失=空状態、タブ消失=削除済み表示、別物=未読復元、を status で受けて反映する。
   */
  async function restoreOnStartup(): Promise<void> {
    let outcome;
    try {
      outcome = await restoreAppState();
    } catch {
      return; // 復元できなくても空状態で起動する（クラッシュさせない）。
    }
    // 未知バージョン/破損は上書き禁止フラグを立てて以後の保存を控える。
    state.safeEmpty = outcome.safe_empty;
    if (state.safeEmpty) {
      // 前回状態を読めなかったため空で起動した／元の state.json は保全し上書きしない旨を伝える
      // （eval medium: 破損空起動の可視化。回復の手掛かりに触れる）。
      notify(
        "前回の状態を読み込めなかったため空の状態で起動しました（元の設定は保全し上書きしません）",
        "warn",
      );
    }
    if (outcome.workspace_status === "restore" && outcome.workspace_path) {
      try {
        const entries = await openWorkspace(outcome.workspace_path);
        state.folder = outcome.workspace_path;
        state.treeEntries = entries;
        resetTreeExpansion();
        state.unread = new UnreadStore();
        ctx.refreshTree();
        ctx.updateTreeHeader();
        // フォルダ名＋件数はツリーヘッダ（T3）へ移したのでステータスからは外す。タブ復元・活性化後に
        // refreshStatus（activateTab 経由）が構造化ステータスを描画する。
        setStatus("");
      } catch {
        // ワークスペースが開けなければ空状態へ落とす（安全遷移）。
      }
    }
    // タブ復元（消失=削除済みタブとして残す・別物=未読復元・正常=位置復元して開く）。
    for (const rt of outcome.tabs) {
      const name = basename(rt.tab.path);
      if (rt.status === "deleted") {
        // 外部削除されたタブを取消線タブとして残す（退避から「確認済み時点に戻す」へ到達可能・eval high）。
        const tab = ctx.newTab(rt.tab.path, name);
        tab.deleted = true;
        tab.cursorLine = rt.tab.cursor_line || 1;
        tab.cursorColumn = rt.tab.cursor_column || 1;
        tab.scrollTop = rt.tab.scroll_top || 1;
        tab.contentHash = rt.tab.content_hash;
        state.unread.apply([{ kind: "removed", path: rt.tab.path }]);
        if (!state.tabs.some((t) => t.path === tab.path)) state.tabs.push(tab);
        continue;
      }
      try {
        await ctx.openFile({ name, path: rt.tab.path, is_dir: false });
        // 保存時の位置を復元する（カーソル/スクロール・eval high の実値復元）。
        restoreTabPosition(rt.tab.path, rt.tab.cursor_line, rt.tab.cursor_column, rt.tab.scroll_top);
        if (rt.status === "unread") {
          // 別物（外部変更）＝未読として復元（差分マークを付ける）。
          state.unread.apply([{ kind: "modified", path: rt.tab.path }]);
        }
      } catch {
        // 個別タブが開けなくても他タブの復元は続ける。
      }
    }
    // 保存時のアクティブタブをパスで再アクティブ化する（復元順に依らない・eval high）。
    if (outcome.active_path && state.tabs.some((t) => t.path === outcome.active_path)) {
      await ctx.activateTab(outcome.active_path);
    }
    ctx.refreshTabs();
    ctx.refreshTree();
  }

  /** 復元したタブの保存時カーソル/スクロールをエディタへ反映する（アクティブタブのみ即時・要件10.1）。 */
  function restoreTabPosition(path: string, line: number, column: number, scrollTop: number): void {
    const tab = state.tabs.find((t) => t.path === path);
    if (tab) {
      tab.cursorLine = line || 1;
      tab.cursorColumn = column || 1;
      tab.scrollTop = scrollTop || 1;
    }
    // 現在エディタに乗っているタブなら即座に位置を反映する（非アクティブタブは activate 時に反映）。
    if (state.active === path && state.editor) {
      state.editor.gotoPosition(line || 1, column || 1);
      state.editor.scrollToLine(scrollTop || 1);
    }
  }

  return { persistAppState, persistAppStateNow, restoreOnStartup, cancelPendingPersist };
}
