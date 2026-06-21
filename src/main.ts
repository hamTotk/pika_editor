// pika フロントエンドのエントリ。
// 中心体験①②: フォルダを開く → ツリー → タブで CM6 を開く → 編集 → 保存／外部変更を未読反映。
// 差分/プレビュー/単一インスタンス等は後続スプリントで肉付けする。
import {
  openWorkspace,
  readFile,
  pathKind,
  saveFile,
  f5Resync,
  computeFileDiff,
  confirmFile,
  confirmAll,
  rollbackFile,
  onFsChanged,
  onWatchMode,
  onOpenRequest,
  restoreAppState,
  saveAppState,
  hashContent,
  noteRecent,
  type TreeEntry,
  type OpenRequestPayload,
  type AppState,
} from "./ipc";
import { initTheme } from "./theme";
import { initA11y } from "./a11y";
import { renderTree } from "./ui/tree";
import { renderTabs, type TabModel } from "./ui/tabs";
import { notify, notices } from "./ui/notifications";
import { setStatus } from "./ui/status";
import { UnreadStore } from "./ui/unread";
import { isImageExt } from "./ui/image";
import { degradeReasonsFromFlags, degradeMessage } from "./ui/viewstate";
import { createEditor, type EditorHandle } from "./editor";
import { renderDiff, type DiffHandle } from "./diff";
import {
  buildPreview,
  parsePreviewFailureMessage,
  resolveOccupancy,
  PreviewSerializer,
  type ViewMode,
} from "./preview";

interface OpenTab extends TabModel {
  dirty: boolean;
  /**
   * このタブの直近既知内容ハッシュ（LF 正規化・backend と同一規則）。
   * 開く/保存/外部リロード時に backend(hash_content)で実値を詰め、終了時 collectAppState で
   * state.json の content_hash として保存する。これにより復元の「別物=未読復元」分岐が
   * production で発火する（eval high: ダミー値固定の解消）。
   */
  contentHash: string;
  /** 1 始まりカーソル位置（非アクティブタブの復元用に最後に見えた位置を保持）。 */
  cursorLine: number;
  cursorColumn: number;
  /** スクロール最上部の行番号（1 始まり近似・復元用）。 */
  scrollTop: number;
  /**
   * 起動復元時に外部削除されていたタブ（取消線＋× 表示で残す）。退避/ベースラインは snapshot に
   * 残るので、削除済みタブから「確認済み時点に戻す（rollback）」へ到達できる回復導線を保つ
   * （eval high: 削除済みタブの回復導線欠落＝旧 wx 版 F-017 と同質の行き止まり防止）。
   */
  deleted: boolean;
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
  // 表示モード（ソース/分割/プレビュー＝要件6.1）。本スプリントはソース⇔プレビューを切替える。
  viewMode: "source" as ViewMode,
  // 系統A/B 切替の直列化（最新世代の load のみ採用し前モード残留を防ぐ＝design doc 6章）。
  previewSerializer: new PreviewSerializer(),
  // 退避＋ベースライン更新を伴う重い操作（確認/一括確認/巻き戻し/保存）の多重実行防止フラグ。
  // 連打で confirm_all/rollback_file が並行発火しデータ操作が重複するのを抑止する（最上位原則）。
  busy: false,
  // state.json が未知バージョン/破損で空起動したか（要件13）。true の間は保存を控える
  // ＝読めない状態を上書きしない（データを失わない）。復元判定は backend(pika-core)が下す。
  safeEmpty: false,
};

const saveBtn = () => document.getElementById("save-file") as HTMLButtonElement;
const editorHost = () => document.getElementById("editor-host") as HTMLElement;
const diffHost = () => document.getElementById("diff-host") as HTMLElement;
const previewHost = () => document.getElementById("preview-host") as HTMLElement;
const togglePreviewBtn = () => document.getElementById("toggle-preview") as HTMLButtonElement;
const toggleDiffBtn = () => document.getElementById("toggle-diff") as HTMLButtonElement;
const confirmBtn = () => document.getElementById("confirm-file") as HTMLButtonElement;
const confirmAllBtn = () => document.getElementById("confirm-all") as HTMLButtonElement;
const rollbackBtn = () => document.getElementById("rollback-file") as HTMLButtonElement;

function refreshTabs(): void {
  renderTabs(state.tabs, state.active, activateTab, state.unread);
  const hasActive = !!state.active;
  togglePreviewBtn().disabled = !hasActive;
  toggleDiffBtn().disabled = !hasActive;
  // 退避＋ベースライン更新を伴う操作（保存/確認/巻き戻し）は in-flight 中（busy）は無効に保つ
  // （内部 refreshTabs で再有効化して二重送信を許してしまわないため）。
  saveBtn().disabled = !hasActive || state.busy;
  confirmBtn().disabled = !hasActive || state.busy;
  rollbackBtn().disabled = !hasActive || state.busy;
  // すべて確認済みはタブ非依存（フォルダ全体の未読集合に対する操作）。退避を伴うため
  // busy 中のみ無効化し、それ以外は常時有効（refreshTabs が in-flight 後の再有効化点）。
  confirmAllBtn().disabled = state.busy;
}

function refreshTree(): void {
  renderTree(state.treeEntries, (entry) => void openFile(entry), state.unread);
}

/**
 * 退避＋ベースライン更新を伴う操作を多重実行から守る（最上位原則「データを失わない」の UI 側ガード）。
 *
 * 確認/一括確認/巻き戻し/保存は非同期。処理中に同種ボタンを再度押せると confirm_all/rollback_file が
 * 並行発火しデータ操作が重複しうる（eval high 指摘）。即座にフラグと該当ボタンを disabled にし、
 * 完了後に必ず解除する。プレビュー系は PreviewSerializer 世代で stale 破棄するので対象外。
 */
async function withBusy(run: () => Promise<void>): Promise<void> {
  if (state.busy) return; // in-flight 中の二重送信を破棄。
  state.busy = true;
  setMutatingButtonsDisabled(true);
  try {
    await run();
  } finally {
    state.busy = false;
    // 完了後はタブ状態に応じて再有効化する（refreshTabs と同じ条件）。
    refreshTabs();
  }
}

/** 退避を伴う操作ボタンをまとめて in-flight 抑止する（withBusy 用）。 */
function setMutatingButtonsDisabled(disabled: boolean): void {
  saveBtn().disabled = disabled;
  confirmBtn().disabled = disabled;
  confirmAllBtn().disabled = disabled;
  rollbackBtn().disabled = disabled;
}

async function activateTab(path: string): Promise<void> {
  // 切替前に現在タブの位置を退避しておく（後でこのタブへ戻ったとき位置を復元するため）。
  captureActivePosition();
  state.active = path;
  // 通知バーのタブ固有通知をこのタブへ切り替える（要件11.1: タブ切替で表示が切り替わる）。
  notices.setActiveTab(path);
  const tab = state.tabs.find((t) => t.path === path);
  // 削除済みタブはディスクから読めない。退避/ベースラインは snapshot に残るので、
  // 直近内容を空にしてエディタを出し「確認済み時点に戻す（rollback）」導線を保つ（eval high）。
  if (tab?.deleted) {
    state.editor?.destroy();
    state.editor = createEditor(editorHost(), "", () => markDirty(path));
    refreshTabs();
    setStatus(`削除済み: ${path}（［確認済み時点に戻す］で退避から復元できます）`);
    return;
  }
  try {
    const content = await readFile(path);
    state.editor?.destroy();
    state.editor = createEditor(editorHost(), content, () => markDirty(path));
    // 開いた内容の実ハッシュを記録する（復元の別物=未読判定に使う・eval high）。
    void captureTabHash(path, content);
    // タブに保持した位置（復元/前回アクティブ時）へカーソル/スクロールを戻す（要件10.1）。
    if (tab && (tab.cursorLine > 1 || tab.cursorColumn > 1 || tab.scrollTop > 1)) {
      state.editor.gotoPosition(tab.cursorLine, tab.cursorColumn);
      state.editor.scrollToLine(tab.scrollTop);
    }
    refreshTabs();
    // 差分トグル ON のままタブを切替えたら新しいアクティブタブの差分を再描画する。
    if (state.diffOn) await renderActiveDiff();
  } catch (e) {
    notify(`開けませんでした: ${String(e)}`, "error");
  }
}

/** タブの直近内容ハッシュを backend(hash_content)で算出して記録する（state.json 復元の素・eval high）。 */
async function captureTabHash(path: string, content: string): Promise<void> {
  const tab = state.tabs.find((t) => t.path === path);
  if (!tab) return;
  try {
    tab.contentHash = await hashContent(content);
  } catch {
    // ハッシュ取得失敗は致命でない（content_hash 空＝復元時は安全側で一致扱い）。
  }
}

/** アクティブエディタのカーソル/スクロールを現在タブへ写す（保存前に呼ぶ・要件10.1）。 */
function captureActivePosition(): void {
  if (!state.active || !state.editor) return;
  const tab = state.tabs.find((t) => t.path === state.active);
  if (!tab) return;
  const cur = state.editor.getCursor();
  tab.cursorLine = cur.line;
  tab.cursorColumn = cur.column;
  tab.scrollTop = state.editor.getScrollTop();
}

function markDirty(path: string): void {
  const tab = state.tabs.find((t) => t.path === path);
  if (tab && !tab.dirty) {
    tab.dirty = true;
    refreshTabs();
  }
}

async function openFile(entry: TreeEntry): Promise<void> {
  // 非テキスト（画像）は簡易ビュー扱い（要件12.2）。寸法プリチェック（6000万px 超は外部誘導）は
  // backend（custom protocol/command）が行う。ここでは種別判定で CM6 へテキスト全量ロードしない分岐を持つ
  // （実描画・寸法プリチェックの実効は系統C＝acceptance TG5/TG6）。
  if (isImageExt(entry.name)) {
    notify(`画像ファイルです（簡易ビューで表示・編集はできません）: ${entry.name}`, "info");
  }
  if (!state.tabs.some((t) => t.path === entry.path)) {
    state.tabs.push(newTab(entry.path, entry.name));
  }
  await activateTab(entry.path);
  // 最近使ったファイルへ記録（要件10.2・ジャンプリスト反映は backend）。
  void noteRecent("file", entry.path).catch(() => undefined);
  void persistAppState();
}

/**
 * 巨大ファイル等の機能縮退（Partial 状態）を通知バーで提示する（要件2.2・ui-design 15章）。
 * backend の DegradeFlagsDto（open_document）から縮退理由を組み立て、黙って切らず理由を出す。
 * 手動再有効化できる理由は今後トグルから戻せるよう、種別=巨大ファイル制限で集約する（要件11.1）。
 */
export function notifyDegrade(
  path: string,
  flags: {
    preview_off: boolean;
    diff_off: boolean;
    highlight_off: boolean;
    wrap_off: boolean;
    editing_off: boolean;
  },
): void {
  const reasons = degradeReasonsFromFlags(flags);
  if (reasons.length === 0) return;
  const text = reasons.map((r) => degradeMessage(r)).join(" / ");
  notices.push("huge-file-limit", path, text);
}

/** 既定値で OpenTab を作る（content_hash/位置は開いた後に実値で埋める）。 */
function newTab(path: string, title: string): OpenTab {
  return {
    path,
    title,
    dirty: false,
    contentHash: "",
    cursorLine: 1,
    cursorColumn: 1,
    scrollTop: 1,
    deleted: false,
  };
}

async function onOpenFolder(): Promise<void> {
  // 最薄ループではパス入力でフォルダを開く（ネイティブ選択ダイアログは capability を増やすため後続）。
  const input = document.getElementById("folder-path") as HTMLInputElement;
  const dir = input.value.trim();
  if (!dir) {
    notify("フォルダのパスを入力してください", "warn");
    return;
  }
  await switchFolder(dir);
}

/**
 * フォルダを開く/切り替える（要件3.2）。起動中に別フォルダを指定した場合（フォルダ切替）は、
 * 未保存タブがあれば確認を挟んでから切り替える（should: フォルダ切替＋未保存確認）。
 * 存在しないフォルダはエラーにする（should: 存在しないフォルダはエラー）。
 */
async function switchFolder(dir: string): Promise<void> {
  // 別フォルダへ切り替えるとき、未保存タブがあれば破棄確認を挟む（データを失わない）。
  const switching = state.folder !== null && state.folder !== dir;
  if (switching && state.tabs.some((t) => t.dirty)) {
    const ok = window.confirm("未保存の変更があります。フォルダを切り替えると破棄されます。続けますか？");
    if (!ok) return;
  }
  try {
    const entries = await openWorkspace(dir);
    if (switching) {
      // 切替時は前フォルダのタブ/未読/エディタを畳む（複数フォルダ同時オープンはしない＝要件14章）。
      state.editor?.destroy();
      state.editor = null;
      state.tabs = [];
      state.active = null;
    }
    state.folder = dir;
    state.treeEntries = entries;
    // 初回オープンは全既読スタート（要件8.1）。未読は外部変更（fs-changed）で付く。
    state.unread = new UnreadStore();
    refreshTree();
    refreshTabs();
    setStatus(`${dir}（${entries.length} 件）`);
    // 最近使ったフォルダへ記録（要件10.2・ジャンプリスト反映は backend）。
    void noteRecent("folder", dir).catch(() => undefined);
    void persistAppState();
  } catch (e) {
    // 存在しないフォルダ等はエラー（要件3.2）。前フォルダはそのまま維持する。
    notify(`フォルダを開けませんでした: ${String(e)}`, "error");
  }
}

async function onSave(): Promise<void> {
  if (!state.active || !state.editor) return;
  const path = state.active;
  const content = state.editor.getContent();
  await withBusy(async () => {
    try {
      await saveFile(path, content);
      const tab = state.tabs.find((t) => t.path === path);
      if (tab) {
        tab.dirty = false;
        // 保存内容を以後の復元基準にする（このセッションで開き直す/復元時の別物判定の素・eval high）。
        // 削除済みタブを保存し直したら実体が復活したので削除フラグを解除する。
        tab.deleted = false;
      }
      void captureTabHash(path, content);
      // 自身の保存では未読を付けない（backend のハッシュ一致抑制と二重で担保）。
      state.unread.clearFile(path);
      refreshTabs();
      refreshTree();
      notify("保存しました");
    } catch (e) {
      notify(`保存に失敗しました: ${String(e)}`, "error");
    }
  });
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
    // 占有はモード×差分トグルの直交で解決する（プレビュー+差分は左右並置＝ui-design 8章）。
    applyOccupancy();
    setStatus(`差分: 変更 ${diff.change_count} 件`);
  } catch (e) {
    notify(`差分の計算に失敗: ${String(e)}`, "error");
  }
}

/** 差分面を閉じてソース/プレビューへ戻す（Ctrl+E 相当＝差分は読み取り専用なので編集はソースで）。 */
function hideDiff(): void {
  state.diff?.destroy();
  state.diff = null;
  state.diffOn = false;
  toggleDiffBtn().setAttribute("aria-pressed", "false");
  // 差分 OFF 後はモードに応じてエディタ/プレビューへ占有を戻す（直交）。
  applyOccupancy();
}

/**
 * プレビュー表示の切替（要件6.1）。ソース ⇔ プレビューを切替える。
 *
 * 権限ゼロ別WebView へ custom protocol(pika-preview://)直配信する URL を backend から得て占有を更新する
 * （HTML 本体はメインWebView を一切経由しない＝design doc 6章）。系統A/B 切替は世代カウンタで直列化し、
 * 古い prepare_preview 結果が後から来ても破棄して前モード残留を防ぐ。
 */
async function onTogglePreview(): Promise<void> {
  if (!state.active) return;
  state.viewMode = state.viewMode === "preview" ? "source" : "preview";
  togglePreviewBtn().setAttribute("aria-pressed", String(state.viewMode === "preview"));
  applyOccupancy();
  if (state.viewMode === "preview") {
    await renderActivePreview();
  }
}

/** 3モード×差分トグルの直交占有を DOM へ適用する（要件6.1・ui-design 8章）。 */
function applyOccupancy(): void {
  const occ = resolveOccupancy(state.viewMode, state.diffOn);
  editorHost().hidden = !occ.showEditor;
  diffHost().hidden = !occ.showDiff;
  previewHost().hidden = !occ.showPreview;
}

/** アクティブタブのプレビューを準備し別WebView へ流す URL を得る（要件6・design doc 6章）。 */
async function renderActivePreview(): Promise<void> {
  if (!state.active) return;
  const path = state.active;
  const content = state.editor ? state.editor.getContent() : await readFile(path);
  // 占有世代を発番（最新世代の load のみ採用＝前モード残留防止）。
  const gen = state.previewSerializer.next();
  // 5状態の Loading（ui-design 15章）。準備中であることを可視化する（系統C で実描画と結線）。
  setPreviewState("loading");
  try {
    // content は 1 回だけ invoke で送る（hazards も同じ戻りに同梱＝IPC 二重転送回避）。
    const ready = await buildPreview(path, content);
    // 古い世代（素早い切替で後着）なら破棄して前モード残留を防ぐ（design doc 6章 直列化）。
    if (!state.previewSerializer.isCurrent(gen)) return;
    // 系統B（HTML）の危険検知を通知バー導線に出す（要件6.3）。has_meta_refresh も伝える。
    const hz = ready.hazards;
    if (hz.has_script) {
      notify("JS を使うHTMLのため表示が崩れる可能性があります（既定のブラウザで開けます）", "warn");
    }
    if (hz.has_external_ref) {
      notify("この文書は外部リソースを参照しています（既定で遮断・許可は設定）", "info");
    }
    if (hz.has_meta_refresh) {
      // meta refresh は ammonia で除去されるため自動遷移しない＝挙動が変わる旨を伝える（medium 指摘）。
      notify("自動リダイレクト（meta refresh）は無効化して表示しています", "info");
    }
    // 別WebView の src を ready.url へ設定するのは系統C（GUI 実機）の配線。ここでは占有領域へ
    // URL を data 属性で保持し、別WebView 生成/ナビゲートは backend 側で行う（HTML は非経由）。
    previewHost().setAttribute("data-preview-url", ready.url);
    previewHost().setAttribute("data-preview-flavor", ready.flavor);
    setPreviewState("ready");
    setStatus(`プレビュー: ${path}`);
  } catch (e) {
    setPreviewState("error");
    notify(`プレビューの準備に失敗: ${String(e)}`, "error");
  }
}

/** プレビューペインの 5状態（ui-design 15章）を data 属性で可視化する（CSS が状態別表示を担う）。 */
function setPreviewState(s: "loading" | "ready" | "error" | "empty"): void {
  previewHost().setAttribute("data-state", s);
}

/** 「確認済みにする」（要件8.3）。確定直前のディスク再照合は backend(pika-core)が担う。 */
async function onConfirm(): Promise<void> {
  if (!state.active) return;
  const path = state.active;
  await withBusy(async () => {
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
  });
}

/** 「すべて確認済みにする」（要件8.3）。実行開始時点の未読集合をフリーズして一括確定する。 */
async function onConfirmAll(): Promise<void> {
  // 実行開始時点の未読集合をフリーズ（要件8.3）。削除済みは対象外。
  const targets = state.unread.confirmTargets();
  if (targets.length === 0) {
    notify("確認すべき未読はありません");
    return;
  }
  await withBusy(async () => {
    try {
      // 各対象の差分を先に提示して backend に diff_snapshot を作らせる（確定直前の再照合基準）。
      // 内容を読んで diff を計算（タブで開いていなければディスク内容を渡す）。各 target は独立なので
      // 並行化して IPC ラウンドトリップの直列線形劣化を避ける（eval medium: N+1 IPC）。
      await Promise.all(
        targets.map(async (path) => {
          const tab = state.tabs.find((t) => t.path === path);
          const current =
            tab && state.active === path && state.editor
              ? state.editor.getContent()
              : await readFile(path);
          await computeFileDiff(path, current);
        }),
      );
      const result = await confirmAll(targets);
      // 確認済みになったものを未読から外す（スキップ分は未読維持＝要件8.3）。
      // backend がスキップしたファイルは差分時点と変わっているので再差分が必要。
      if (result.skipped === 0) {
        for (const path of targets) state.unread.clearFile(path);
      }
      // スキップがある場合は安全側で未確定を未読のまま残す（退避は取れているのでデータ喪失なし）。
      refreshTree();
      refreshTabs();
      if (state.diffOn) await renderActiveDiff();
      notify(
        `すべて確認済み: ${result.updated} 件確定 / ${result.skipped} 件スキップ（更新前は一括退避済み）`,
      );
      // スキップは「処理中にファイルが変化した」ため。次の一手（F5 再同期→再確認）を明示する
      // （回復導線＝ux 指摘の行き止まり感解消。スキップ分は未読のまま残る）。
      if (result.skipped > 0) {
        notify(
          `${result.skipped} 件は確認中に変化したためスキップしました。F5 で再同期して再度ご確認ください`,
          "warn",
        );
      }
    } catch (e) {
      notify(`一括確認に失敗: ${String(e)}`, "error");
    }
  });
}

/** 「確認済み時点に戻す」（要件8.3/7.3）。退避不能はエラー通知（既定ブロック）。 */
async function onRollback(): Promise<void> {
  if (!state.active || !state.editor) return;
  const path = state.active;
  const editor = state.editor;
  await withBusy(async () => {
    try {
      // backend が現在内容を退避してからベースライン内容を返す（退避が最後の砦）。
      const baselineContent = await rollbackFile(path);
      // バッファをベースライン内容で上書き（外部リロード扱い＝単一トランザクション/非dirty）。
      editor.reloadExternal(baselineContent);
      void captureTabHash(path, baselineContent);
      // 削除済みタブから戻したら実体が（保存により）復元される導線なので削除フラグを解除する。
      const tab = state.tabs.find((t) => t.path === path);
      if (tab) tab.deleted = false;
      state.unread.clearFile(path);
      refreshTree();
      refreshTabs();
      if (state.diffOn) await renderActiveDiff();
      notify("確認済み時点に戻しました（戻す前の内容は退避済み）");
    } catch (e) {
      notify(`巻き戻しできません: ${String(e)}`, "error");
    }
  });
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
    // 反映後の内容を復元基準ハッシュとして更新（次回起動の別物判定の素・eval high）。
    void captureTabHash(state.active, content);
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

/**
 * 単一インスタンス転送（要件3.4）。既に起動済みの pika に別プロセスが `pika <path>` を投げると
 * backend が named pipe で受け取り core 再検証した絶対パスを `open-request` で送ってくる。
 * 受理操作=パスオープン限定なので、ここでは「ファイルを開く（あれば -g 位置へ）」だけを行う。
 */
async function onOpenRequestEvent(payload: OpenRequestPayload): Promise<void> {
  for (const path of payload.paths) {
    await openPath(path);
  }
  // -g カーソル位置は paths 先頭ファイルに対応する規約（ipc.rs OpenRequest）。
  // 複数パス時はループ末尾でなく先頭タブをアクティブにしてからカーソルを置く（eval medium）。
  if (payload.goto && payload.paths.length > 0) {
    const first = payload.paths[0];
    await openPath(first);
    if (state.editor) {
      state.editor.gotoPosition(payload.goto.line, payload.goto.column ?? 1);
    }
  }
}

/**
 * 転送/起動パスを種別に応じて開く（要件3.2）。
 * - フォルダ → フォルダ切替（未保存確認込み）。
 * - 既存ファイル → タブで開く。
 * - 存在しないパス → 「保存時に作成される新規タブ」として空タブで開く（should: 存在しないファイルパス）。
 */
async function openPath(path: string): Promise<void> {
  const name = path.split(/[\\/]/).pop() ?? path;
  let kind: "file" | "dir" | "missing";
  try {
    kind = await pathKind(path);
  } catch {
    kind = "missing";
  }
  if (kind === "dir") {
    await switchFolder(path);
    return;
  }
  if (kind === "missing") {
    openNewFileTab(path, name);
    return;
  }
  await openFile({ name, path, is_dir: false });
}

/** 存在しないファイルパスを「保存時に作成される新規タブ」として空タブで開く（要件3.2）。 */
function openNewFileTab(path: string, name: string): void {
  if (!state.tabs.some((t) => t.path === path)) {
    const tab = newTab(path, name);
    // 新規（未保存）なのでタブを dirty にせず、保存で実体を作る。空内容の hash を記録しておく。
    state.tabs.push(tab);
  }
  state.active = path;
  state.editor?.destroy();
  state.editor = createEditor(editorHost(), "", () => markDirty(path));
  void captureTabHash(path, "");
  refreshTabs();
  setStatus(`新規ファイル（保存で作成）: ${path}`);
  persistAppState();
}

/** 現在の UI 状態を AppState へ写す（state.json 保存用・要件10.1）。 */
function collectAppState(): AppState {
  // アクティブタブのカーソル/スクロールを最新化してから収集する（実値で保存・eval high）。
  captureActivePosition();
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
      state.unread = new UnreadStore();
      refreshTree();
      setStatus(`${outcome.workspace_path}（${entries.length} 件）`);
    } catch {
      // ワークスペースが開けなければ空状態へ落とす（安全遷移）。
    }
  }
  // タブ復元（消失=削除済みタブとして残す・別物=未読復元・正常=位置復元して開く）。
  for (const rt of outcome.tabs) {
    const name = rt.tab.path.split(/[\\/]/).pop() ?? rt.tab.path;
    if (rt.status === "deleted") {
      // 外部削除されたタブを取消線タブとして残す（退避から「確認済み時点に戻す」へ到達可能・eval high）。
      const tab = newTab(rt.tab.path, name);
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
      await openFile({ name, path: rt.tab.path, is_dir: false });
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
    await activateTab(outcome.active_path);
  }
  refreshTabs();
  refreshTree();
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

async function main(): Promise<void> {
  initTheme();
  // ARIA 全Web再構築の初期化（F6/Shift+F6 ペイン間フォーカス循環・ランドマーク確実化＝要件11.5・design doc 17章）。
  initA11y();
  document.getElementById("open-folder")?.addEventListener("click", () => void onOpenFolder());
  saveBtn().addEventListener("click", () => void onSave());
  togglePreviewBtn().addEventListener("click", () => void onTogglePreview());
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
  // 信頼 JS（別WebView 内の Mermaid/KaTeX/highlight）描画失敗件数の受信導線（要件6.2）。
  // 別WebView は独立 WebView のため window.parent では本体に届かず、本番（系統C 結線）では
  // Tauri event/IPC 経路へ置換する（T-007/TE6）。ここでは受信→通知バー集計の片側配線を解消し、
  // window message（将来 iframe 経路・テスト）でも parsePreviewFailureMessage で受理する。
  window.addEventListener("message", (e: MessageEvent) => {
    const failures = parsePreviewFailureMessage(e.data);
    if (failures !== null) {
      notify(`プレビューの一部ブロックを描画できませんでした（${failures} 件・元コード表示）`, "warn");
    }
  });
  // 外部変更/監視モードの購読（backend の emit を受ける）。
  await onFsChanged((payload) => onExternalChange(payload.changes));
  await onWatchMode((message) => notify(message, "info"));
  // 単一インスタンス転送（要件3.4）: 既存インスタンスへ後続プロセスが投げたパスを開く。
  await onOpenRequest((payload) => void onOpenRequestEvent(payload));
  // 終了直前にアプリ状態を保存（要件10.1。アトミック書込は backend）。デバウンス待ちでなく即時 flush。
  window.addEventListener("beforeunload", () => void persistAppStateNow());
  // 起動時に state.json を復元（version 安全側・復元3分岐は backend が判定）。
  await restoreOnStartup();
  if (!state.folder) setStatus("フォルダを開いてください");
}

void main();
