// pika フロントエンドのエントリ。
// 中心体験①②: フォルダを開く → ツリー → タブで CM6 を開く → 編集 → 保存／外部変更を未読反映。
// 差分/プレビュー/単一インスタンス等は後続スプリントで肉付けする。
import {
  openWorkspace,
  listDir,
  readFile,
  pathKind,
  openDocument,
  saveDocument,
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
  showPreview,
  hidePreview,
  setPreviewBounds,
  type TreeEntry,
  type OpenRequestPayload,
  type AppState,
  type DocEncoding,
  type PreviewRect,
} from "./ipc";
import { initTheme } from "./theme";
import { initA11y, announce } from "./a11y";
import { resolveShortcut, modsOf, normalizeKey, type Action, type Focus } from "./shortcuts";
import { renderTree, resetTreeExpansion } from "./ui/tree";
import { renderTabs, type TabModel } from "./ui/tabs";
import { notify, notices } from "./ui/notifications";
import { setStatus } from "./ui/status";
import { UnreadStore } from "./ui/unread";
import { isImageExt } from "./ui/image";
import { degradeReasonsFromFlags, degradeMessage, emptyMessage } from "./ui/viewstate";
import { createEditor, type EditorHandle } from "./editor";
import { renderDiff, type DiffHandle } from "./diff";
import {
  buildPreview,
  resolveOccupancy,
  resolvePreviewTheme,
  PreviewSerializer,
  type ViewMode,
} from "./preview";
import type { PreviewTheme } from "./ipc";

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
  /**
   * 開いたときに検出した元エンコーディング（保存時に維持する＝要件5.2・eval medium）。
   * open_document が判定した値。これを save_document に渡し、Shift_JIS 等が暗黙に UTF-8 化
   * されるのを防ぐ（最上位原則「データを失わない」）。新規タブ/不明時は utf-8。
   */
  encoding: DocEncoding;
  /** 開いたときに BOM があったか（保存時に維持する＝要件5.2）。 */
  hasBom: boolean;
  /**
   * このタブで「許可して再読込」したオプトイン外部許可ホスト（要件6.2/6.3・2.4）。
   * 既定は undefined（外部遮断）。許可は **タブ単位** で保持し、別タブ/別文書では既定オフに戻る
   * （永続はしない＝要件6.2「既定は必ずオフに戻る」）。renderActivePreview がこれを buildPreview へ渡す。
   */
  allowExternal?: string[];
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
const workbench = () => document.getElementById("workbench") as HTMLElement;
const treeHeadLabel = () => document.getElementById("tree-head-label") as HTMLElement;
const treeCollapseBtn = () => document.getElementById("tree-collapse") as HTMLButtonElement;
const treeExpandBtn = () => document.getElementById("tree-expand") as HTMLButtonElement;
const editorPane = () => document.getElementById("editor-pane") as HTMLElement;
const editorHost = () => document.getElementById("editor-host") as HTMLElement;
const diffHost = () => document.getElementById("diff-host") as HTMLElement;
const previewHost = () => document.getElementById("preview-host") as HTMLElement;
const togglePreviewBtn = () => document.getElementById("toggle-preview") as HTMLButtonElement;
const toggleDiffBtn = () => document.getElementById("toggle-diff") as HTMLButtonElement;
const confirmBtn = () => document.getElementById("confirm-file") as HTMLButtonElement;
const confirmAllBtn = () => document.getElementById("confirm-all") as HTMLButtonElement;
const rollbackBtn = () => document.getElementById("rollback-file") as HTMLButtonElement;

function refreshTabs(): void {
  renderTabs(state.tabs, state.active, activateTab, state.unread, closeTab);
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
  // 子フォルダの遅延展開は副作用なし列挙（listDir）で取得する（監視ルート付け替え/ベースライン再取得をしない）。
  renderTree(state.treeEntries, (entry) => void openFile(entry), listDir, state.unread);
}

/**
 * ツリーヘッダ（B4・ui-design 7章）の表示を現在のフォルダに合わせて更新する。
 * 「エクスプローラー — <フォルダ名>」を出す。未オープン時は「エクスプローラー」のみ。
 * フォルダ名は state.folder（絶対パス）の末尾区切り以降（ベース名）を使う（末尾の \ や / は除く）。
 */
function updateTreeHeader(): void {
  const label = treeHeadLabel();
  const folder = state.folder;
  if (!folder) {
    label.textContent = "エクスプローラー";
    label.removeAttribute("title");
    return;
  }
  // 末尾の区切り文字を落としてからベース名を取る（C:\work\notes\ → notes、ドライブ直下 C:\ → C:）。
  const trimmed = folder.replace(/[\\/]+$/, "");
  const base = trimmed.split(/[\\/]/).pop() || trimmed;
  label.textContent = `エクスプローラー — ${base}`;
  // 省略表示でも全パスが分かるよう title（ツールチップ）に絶対パスを残す。
  label.setAttribute("title", folder);
}

/**
 * ツリーの収納/引き出し（B5・ui-design 7章）。
 * #workbench に tree-collapsed を付け外しして左ペインをレールへ畳む/戻す。
 * a11y: 収納/引き出しボタンの aria-expanded を実状態へ同期し、収納時は引き出しボタン（レール）へ、
 * 引き出し時は収納ボタンへフォーカスを移して、キーボードのみでも操作位置を見失わないようにする
 * （要件11.5: 収納してもキーボードで引き出しに到達できる）。
 */
function setTreeCollapsed(collapsed: boolean): void {
  workbench().classList.toggle("tree-collapsed", collapsed);
  // 両ボタンの aria-expanded はツリー（aria-controls=tree）の可視状態を表す。
  treeCollapseBtn().setAttribute("aria-expanded", String(!collapsed));
  treeExpandBtn().setAttribute("aria-expanded", String(!collapsed));
  // 収納で消えるボタンにフォーカスが残ると到達不能になるため、見えている側のボタンへ移す。
  if (collapsed) treeExpandBtn().focus();
  else treeCollapseBtn().focus();
  // 左ペインの幅が変わるのでプレビュー別WebView の矩形を追従させる（レイアウト確定後）。
  requestAnimationFrame(() => syncPreviewBounds());
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
  // 外部リソース許可は **タブ切替で必ず既定オフに戻す**（要件6.2「既定は必ずオフに戻る」・永続しない）。
  // 「許可して再読込」はこのタブをアクティブに見ている間だけ有効で、切り替えると遮断へ戻る。
  if (tab) tab.allowExternal = undefined;
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
    // open_document でエンコーディングを判定して開く（保存時に元エンコーディングを維持する＝要件5.2）。
    // 第2段階以降は text が空（仮想化ビューアは系統C）。通常/第1段階のテキストをエディタへ載せる。
    const doc = await openDocument(path);
    const content = doc.text;
    if (tab) {
      // 検出エンコーディング/BOM をタブに保持し、保存時 save_document へ渡す（暗黙 UTF-8 化の防止・eval medium）。
      tab.encoding = doc.encoding;
      tab.hasBom = doc.has_bom;
    }
    state.editor?.destroy();
    state.editor = createEditor(editorHost(), content, () => markDirty(path));
    // 段階制で機能が縮退するとき（preview/diff/highlight 等の自動オフ）は理由を通知バー提示する（要件2.2）。
    notifyDegrade(path, doc.degrade);
    if (doc.decode_warning) {
      notify(`エンコーディングを自動判定できず UTF-8 で開きました: ${tab?.title ?? path}`, "warn");
    }
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
    // プレビュー表示中（preview/split）のままタブを切替えたら、新アクティブタブのプレビューへ追従させる
    // （別WebView を新内容へ再ナビゲート。占有世代で stale 破棄＝design doc 6章）。
    if (resolveOccupancy(state.viewMode, state.diffOn).showPreview) {
      await renderActivePreview();
    }
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

/** 既定値で OpenTab を作る（content_hash/位置/エンコーディングは開いた後に実値で埋める）。 */
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
    // 既定は BOM なし UTF-8（新規ファイル/不明時）。既存ファイルは open 時に open_document の判定で上書きする。
    encoding: "utf-8",
    hasBom: false,
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
 * 未保存タブがある状態での破壊的操作（フォルダ切替）の三択確認（要件5.3・eval medium）。
 * 対象ファイル名を明示し、保存して切替/破棄して切替/キャンセル を選べる。
 *
 * vanilla TS のためネイティブ window.confirm を 2 段にして三択を表現する:
 * 1段目「保存してから切り替えますか？」OK=save / Cancel→2段目
 * 2段目「保存せず破棄して切り替えますか？」OK=discard / Cancel=cancel
 */
function confirmDiscardUnsaved(names: string[]): Promise<"save" | "discard" | "cancel"> {
  // 対象名を列挙（多すぎる場合は先頭数件＋残数）。
  const shown = names.slice(0, 5).join("、");
  const more = names.length > 5 ? ` ほか ${names.length - 5} 件` : "";
  const list = `${shown}${more}`;
  const save = window.confirm(
    `未保存の変更があります（${list}）。\n保存してからフォルダを切り替えますか？\n` +
      `［OK］保存して切替 ／［キャンセル］次の選択へ`,
  );
  if (save) return Promise.resolve("save");
  const discard = window.confirm(
    `保存せずに破棄してフォルダを切り替えますか？（${list}）\n` +
      `［OK］破棄して切替 ／［キャンセル］切替を中止`,
  );
  return Promise.resolve(discard ? "discard" : "cancel");
}

/**
 * フォルダを開く/切り替える（要件3.2）。起動中に別フォルダを指定した場合（フォルダ切替）は、
 * 未保存タブがあれば確認を挟んでから切り替える（should: フォルダ切替＋未保存確認）。
 * 存在しないフォルダはエラーにする（should: 存在しないフォルダはエラー）。
 */
async function switchFolder(dir: string): Promise<void> {
  // 別フォルダへ切り替えるとき、未保存タブがあれば三択（保存して切替/破棄して切替/キャンセル）で確認する
  // （要件5.3 の保存・破棄・キャンセル思想／eval medium: 対象名と第3選択肢を欠かない）。
  const switching = state.folder !== null && state.folder !== dir;
  if (switching) {
    const dirtyTabs = state.tabs.filter((t) => t.dirty);
    if (dirtyTabs.length > 0) {
      const decision = await confirmDiscardUnsaved(dirtyTabs.map((t) => t.title));
      if (decision === "cancel") return;
      if (decision === "save") {
        // 未保存タブをすべて保存してから切り替える（save_document 経由＝退避先・エンコーディング維持）。
        for (const t of dirtyTabs) {
          if (state.active !== t.path) await activateTab(t.path);
          await onSave();
          // 保存が中断/失敗して dirty が残ったら切替を中止する（無確認の喪失をしない）。
          if (state.tabs.find((x) => x.path === t.path)?.dirty) {
            notify(`「${t.title}」を保存できなかったためフォルダ切替を中止しました`, "warn");
            return;
          }
        }
      }
      // "discard" はそのまま切替（破棄）。
    }
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
    // 別フォルダを開いたので前フォルダのツリー展開状態/子キャッシュを破棄する（誤展開防止）。
    resetTreeExpansion();
    // 初回オープンは全既読スタート（要件8.1）。未読は外部変更（fs-changed）で付く。
    state.unread = new UnreadStore();
    refreshTree();
    updateTreeHeader();
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

/**
 * 保存（要件5.2/5.6・eval medium）。エンコーディング非対応の旧 save_file ではなく **save_document** を呼ぶ:
 * - 元エンコーディング/BOM を維持する（Shift_JIS 等の暗黙 UTF-8 化を防ぐ＝データを失わない）。
 * - 破壊的上書きの前にディスク現内容を incoming 退避する（退避が先・取れなければ中断）。
 * - 現エンコーディングで表現不能な文字があれば**保存せず中断**し ［UTF-8で保存/該当文字を確認/キャンセル］を提示する。
 *
 * withBusy で多重実行を防ぐ。表現不能文字で中断したときの「UTF-8で保存」再試行は同じ withBusy 区間内で
 * 行う（withBusy を二重に取らない＝busy フラグで弾かれない・最上位原則の UI ガードと両立）。
 */
async function onSave(): Promise<void> {
  if (!state.active || !state.editor) return;
  const path = state.active;
  const content = state.editor.getContent();
  const tab = state.tabs.find((t) => t.path === path);
  await withBusy(async () => {
    await saveOnce(path, content, tab, false);
  });
}

/**
 * 1 回分の保存処理（withBusy の内側で呼ぶ）。表現不能文字で中断したら確認のうえ UTF-8 で再試行する
 * （同区間内の再帰なので withBusy を取り直さない）。
 */
async function saveOnce(
  path: string,
  content: string,
  tab: OpenTab | undefined,
  forceUtf8: boolean,
): Promise<void> {
  const encoding: DocEncoding = forceUtf8 ? "utf-8" : (tab?.encoding ?? "utf-8");
  const hasBom = forceUtf8 ? false : (tab?.hasBom ?? false);
  try {
    const result = await saveDocument(path, content, encoding, hasBom, forceUtf8);
    if (result.status === "unmappable") {
      // 表現不能文字で中断（要件5.6）。この時点でディスクは未変更＝データは失われていない。
      // ［UTF-8で保存／キャンセル］を確認し、選べば UTF-8（BOM なし）で保存し直す。
      const name = path.split(/[\\/]/).pop() ?? path;
      const ok = window.confirm(
        `「${name}」には現在のエンコーディングで保存できない文字が ${result.unmappable.length} 件あります。\n` +
          `UTF-8（BOM なし）で保存しますか？［キャンセル］を選ぶと保存しません（変更は失われません）。`,
      );
      if (ok) {
        await saveOnce(path, content, tab, true);
      } else {
        notify("保存を中止しました（表現不能文字のため・変更は保持しています）", "warn");
      }
      return;
    }
    if (tab) {
      tab.dirty = false;
      // 保存内容を以後の復元基準にする（このセッションで開き直す/復元時の別物判定の素・eval high）。
      // 削除済みタブを保存し直したら実体が復活したので削除フラグを解除する。
      tab.deleted = false;
      // UTF-8 強制保存を選んだら以後の元エンコーディングを UTF-8（BOM なし）へ確定する。
      if (forceUtf8) {
        tab.encoding = "utf-8";
        tab.hasBom = false;
      }
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
    // 表示専用ステータスとは別に、スクリーンリーダーへ差分件数を polite で読ませる（要件11.5・eval medium）。
    announce(`差分 変更 ${diff.change_count} 件`);
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
  // プレビュー＋差分が両方表示のときは左右並置（左＝レンダリング／右＝テキスト差分＝ui-design 8章）。
  // それ以外（単独占有）は分割クラスを外し、既存の単独レイアウト（grid-row:2 を 1 つが占有）に戻す。
  const split = occ.showPreview && occ.showDiff;
  if (split) {
    editorPane().setAttribute("data-split", "preview-diff");
  } else {
    editorPane().removeAttribute("data-split");
  }
  // 占有がプレビュー非表示なら別WebView を隠す（表示は renderActivePreview の show_preview が担う）。
  // 占有がプレビュー表示でも、ここでは bounds 追従のみ更新する（ナビゲートは renderActivePreview）。
  if (!occ.showPreview) {
    void hidePreview();
  } else {
    // レイアウト変更（分割の付け外し）で #preview-host の矩形が変わるため別WebView を再同期する。
    // ResizeObserver も拾うが、確実に追従させるためここでも明示的に呼ぶ（左半分への移動を即反映）。
    // DOM のレイアウト確定後に矩形を取るため次フレームで実行する（getBoundingClientRect の確定待ち）。
    requestAnimationFrame(() => syncPreviewBounds());
  }
}

/** `#preview-host` の DOM 矩形（メイン窓クライアント領域基準・CSS ピクセル）を子WebView 配置用に取る。 */
function previewRect(): PreviewRect {
  const r = previewHost().getBoundingClientRect();
  return { x: r.left, y: r.top, w: r.width, h: r.height };
}

/** プレビュー別WebView を `#preview-host` の現在矩形へ追従させる（resize・ペイン変化時）。 */
function syncPreviewBounds(): void {
  // 占有がプレビュー表示で、領域が確定（幅高さ>0）のときのみ追従させる（非表示中は無害化）。
  const occ = resolveOccupancy(state.viewMode, state.diffOn);
  if (!occ.showPreview || previewHost().hidden) return;
  const rect = previewRect();
  if (rect.w <= 0 || rect.h <= 0) return;
  void setPreviewBounds(rect);
}

/**
 * メインアプリの解決済みトークン色から別WebView 用テーマを読み取る（Stage ③・design doc 10章）。
 *
 * 別WebView は独立文書でアプリの CSS 変数を継承しないため、`getComputedStyle` で解決済みの
 * 主要トークン色を読み取り backend へ渡して `:root{--pk-*}` 注入させる。ダーク判定は
 * `color-scheme` に "dark" を含むか / `prefers-color-scheme` と data-theme から解決する。
 */
function activePreviewTheme(): PreviewTheme {
  const root = document.documentElement;
  const cs = getComputedStyle(root);
  const getToken = (name: string): string => cs.getPropertyValue(name).trim();
  // ダーク判定: 解決済み color-scheme が "dark" を含むか（data-theme=dark / system+OS ダークで効く）。
  // 念のため data-theme=system のときは prefers-color-scheme も見る（古い WebView の保険）。
  const colorScheme = cs.colorScheme || "";
  let dark = colorScheme.includes("dark");
  if (!dark && root.getAttribute("data-theme") === "system") {
    dark = window.matchMedia("(prefers-color-scheme: dark)").matches;
  }
  return resolvePreviewTheme(getToken, dark);
}

/** アクティブタブのプレビューを準備し別WebView へ流す URL を得る（要件6・design doc 6章）。 */
async function renderActivePreview(): Promise<void> {
  if (!state.active) return;
  const path = state.active;
  const content = state.editor ? state.editor.getContent() : await readFile(path);
  const tab = state.tabs.find((t) => t.path === path);
  // 占有世代を発番（最新世代の load のみ採用＝前モード残留防止）。
  const gen = state.previewSerializer.next();
  // 5状態の Loading（ui-design 15章）。準備中であることを可視化する（系統C で実描画と結線）。
  setPreviewState("loading");
  try {
    // content は 1 回だけ invoke で送る（hazards も同じ戻りに同梱＝IPC 二重転送回避）。
    // テーマ配色を別WebView へ降ろす（Stage ③・design doc 10章）。系統B は backend が無視する。
    // 外部許可（このタブで「許可して再読込」した https ホスト）があれば渡す（要件6.2・既定は空＝遮断）。
    // 最終検証は backend(CSP)が https のみ受理して行う（不正は fail-closed で全破棄）。
    const ready = await buildPreview(path, content, {
      theme: activePreviewTheme(),
      allowExternal: tab?.allowExternal,
    });
    // 古い世代（素早い切替で後着）なら破棄して前モード残留を防ぐ（design doc 6章 直列化）。
    if (!state.previewSerializer.isCurrent(gen)) return;
    // 系統B（HTML）の危険検知を通知バー導線に出す（要件6.3）。has_meta_refresh も伝える。
    const hz = ready.hazards;
    if (hz.has_script) {
      notify("JS を使うHTMLのため表示が崩れる可能性があります（既定のブラウザで開けます）", "warn");
    }
    if (hz.has_external_ref) {
      // 既に許可済み（このタブで「許可して再読込」した）なら通知を消し、未許可なら許可導線を出す。
      // 許可状態はタブ単位・このセッション限り（タブ切替/別文書で既定オフに戻る＝要件6.2）。
      if (tab && tab.allowExternal && tab.allowExternal.length > 0) {
        notices.dismiss("external-resource", path);
      } else {
        // アクション付き通知（［許可して再読込］）。クリックで当該タブの allowExternal に収集ホストを
        // 覚え、プレビューを再描画する（許可された https 画像が表示される＝要件6.2 オプトイン許可）。
        notices.push(
          "external-resource",
          path,
          "この文書は外部リソースを参照しています（既定で遮断）",
          [
            {
              label: "許可して再読込",
              onClick: () => {
                const t = state.tabs.find((x) => x.path === path);
                if (!t) return;
                t.allowExternal = hz.external_hosts;
                // 再読込は当該タブがアクティブ・プレビュー表示中のときのみ意味を持つ。
                if (state.active === path && resolveOccupancy(state.viewMode, state.diffOn).showPreview) {
                  void renderActivePreview();
                }
              },
            },
          ],
        );
      }
    }
    if (hz.has_meta_refresh) {
      // meta refresh は ammonia で除去されるため自動遷移しない＝挙動が変わる旨を伝える（medium 指摘）。
      notify("自動リダイレクト（meta refresh）は無効化して表示しています", "info");
    }
    // 権限ゼロ別WebView を `#preview-host` の矩形へ重ね、ready.url へナビゲートする（HTML は非経由）。
    // URL のみ backend に渡し、HTML 本体は custom protocol が別WebView へ直配信する（design doc 6章）。
    // data 属性は診断/系統C スクショ検証の手掛かりとして残す（実体の表示は show_preview）。
    previewHost().setAttribute("data-preview-url", ready.url);
    previewHost().setAttribute("data-preview-flavor", ready.flavor);
    await showPreview(previewRect(), ready.url);
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
    // Empty 3分岐の「消化後（all-consumed）」文言（ui-design 15章）。
    notify(emptyMessage("all-consumed"));
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
  // ステータスに件数を出す。表示専用ステータスとは別に未読件数を読み上げ領域へ announce する（要件11.5）。
  if (state.folder) {
    const count = state.unread.unreadCount();
    setStatus(`${state.folder}（未読 ${count} 件）`);
    announce(`差分あり ${count} 件`);
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
      resetTreeExpansion();
      state.unread = new UnreadStore();
      refreshTree();
      updateTreeHeader();
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

/**
 * いまフォーカスのあるペインを判定する（Ctrl+Enter 誤爆防止に使う＝要件11.2）。
 * 差分/プレビュー領域にフォーカスがあるときだけ Ctrl+Enter で「確認済み」を発火させる。
 */
function currentFocus(): Focus {
  const active = document.activeElement;
  if (!active) return "other";
  if (document.getElementById("diff-host")?.contains(active)) return "diff";
  if (document.getElementById("preview-host")?.contains(active)) return "preview";
  if (document.getElementById("editor-host")?.contains(active)) return "editor";
  if (document.getElementById("tree")?.contains(active)) return "tree";
  return "other";
}

/**
 * 解決済みショートカット操作（Action）を実際のハンドラへ振り分ける（要件11.2）。
 * 判定（どのキーがどの操作か）は shortcuts.resolveShortcut（pika-core::shortcuts の写し）に集約し、
 * ここは「操作 → ハンドラ」の対応表に徹する。発火したら true（呼び出し側で preventDefault）。
 */
function dispatchAction(action: Action): boolean {
  switch (action) {
    case "open-file":
      void onOpenFile();
      return true;
    case "open-folder":
      void onOpenFolder();
      return true;
    case "toggle-preview":
      // 差分表示中はソースへ戻す（差分は読み取り専用＝要件8.2）。それ以外はソース⇔プレビュー切替。
      if (state.diffOn) hideDiff();
      else void onTogglePreview();
      return true;
    case "toggle-split":
      void onToggleDiff();
      return true;
    case "toggle-diff":
      void onToggleDiff();
      return true;
    case "confirm-file":
      void onConfirm();
      return true;
    case "confirm-all":
      void onConfirmAll();
      return true;
    case "next-change":
      if (state.diff) {
        state.diff.jumpNext();
        return true;
      }
      return false;
    case "prev-change":
      if (state.diff) {
        state.diff.jumpPrev();
        return true;
      }
      return false;
    case "save":
      void onSave();
      return true;
    case "close-tab":
      onCloseActiveTab();
      return true;
    case "resync":
      void onF5();
      return true;
    case "find":
    case "replace": {
      // 検索/置換バー UI はソース/分割向けに用意する（要件5.4）。検索バー実体は系統C（GUI 実機）。
      // ここではキーが死蔵されないよう導線（通知）を出し、core 側 search_in_text/replace_in_text へ繋ぐ枠を持つ。
      const label = action === "find" ? "検索（Ctrl+F）" : "置換（Ctrl+H）";
      notify(`${label}：検索バーは表示メニューから利用できます`, "info");
      return true;
    }
  }
}

/**
 * ファイルを開く（Ctrl+O・要件11.2）。最薄ループではパス入力で開く（ネイティブ選択ダイアログは
 * capability を増やすため後続）。フォルダパス入力欄を流用し、ファイルパスなら開く。
 */
async function onOpenFile(): Promise<void> {
  const input = document.getElementById("folder-path") as HTMLInputElement;
  const p = input.value.trim();
  if (!p) {
    notify("開くファイルのパスを入力してください", "warn");
    input.focus();
    return;
  }
  await openPath(p);
}

/** アクティブタブを閉じる（Ctrl+W・要件11.2）。任意パス版 closeTab へ委譲する。 */
function onCloseActiveTab(): void {
  if (!state.active) return;
  closeTab(state.active);
}

/**
 * 任意のタブを閉じる（Ctrl+W／タブの × クリック・要件11.2）。未保存があれば破棄確認を挟む
 * （データを失わない＝確認を必ず通す）。閉じたのがアクティブタブなら隣へアクティブを移す。
 */
function closeTab(path: string): void {
  const tab = state.tabs.find((t) => t.path === path);
  if (!tab) return;
  if (tab.dirty) {
    const name = tab.title;
    const ok = window.confirm(
      `「${name}」に未保存の変更があります。保存せずに閉じると失われます。閉じますか？`,
    );
    if (!ok) return;
  }
  const idx = state.tabs.findIndex((t) => t.path === path);
  const wasActive = state.active === path;
  state.tabs = state.tabs.filter((t) => t.path !== path);
  if (!wasActive) {
    // 非アクティブタブを閉じた場合はアクティブを維持し、タブ列だけ更新する。
    refreshTabs();
    void persistAppState();
    return;
  }
  // アクティブを閉じたので隣のタブへアクティブを移す（無ければエディタを畳む）。
  const next = state.tabs[idx] ?? state.tabs[idx - 1] ?? null;
  if (next) {
    void activateTab(next.path);
  } else {
    state.active = null;
    state.editor?.destroy();
    // 空のエディタを残す（タブが無くてもキーボード操作の到達先を保つ）。
    state.editor = createEditor(editorHost(), "", () => undefined);
    refreshTabs();
    setStatus(emptyMessage("no-folder"));
  }
  void persistAppState();
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
  // ツリー収納/引き出しトグル（B5・ui-design 7章）。ヘッダ右端「‹」で収納、レール「›」で引き出す。
  treeCollapseBtn().addEventListener("click", () => setTreeCollapsed(true));
  treeExpandBtn().addEventListener("click", () => setTreeCollapsed(false));
  // ツリーヘッダ（B4）の初期表示（フォルダ未オープン時は「エクスプローラー」のみ）。復元後に更新される。
  updateTreeHeader();
  // 主要ショートカットを単一のキーディスパッチ表で処理する（要件11.2・eval high: shortcuts 配線）。
  // 判定（どのキーが何の操作か・Ctrl+Enter 誤爆防止・代替割当）は shortcuts.resolveShortcut
  // （pika-core::shortcuts の写し）へ集約し、ここは結果（Action）を dispatchAction へ流すだけ。
  // F6/Shift+F6（ペイン間フォーカス循環）は a11y/index.ts が capture フェーズで先取りする。
  window.addEventListener("keydown", (e) => {
    const action = resolveShortcut(normalizeKey(e), modsOf(e), currentFocus());
    if (action === null) return; // 未割当キーは CM6 等の既定処理へ委ねる。
    if (dispatchAction(action)) e.preventDefault();
  });
  // 信頼 JS（別WebView 内の Mermaid/KaTeX/highlight）描画失敗の扱い（要件6.2・Stage ③）:
  // **失敗は in-preview で可視化する**（pika-core の TRUSTED_JS_INIT が失敗ブロックへ pika-block-error を
  // 付け、別WebView 文書の BASE_CSS が縦線＋「描画に失敗」ラベルで視認可能にする）。
  // F-029 でプレビュー別WebView からの IPC/event は境界で全拒否されるため、「失敗件数をメインへ送る」
  // 経路は持たない（穴を開け直さない＝セキュリティ設計と両立）。よって message リスナは廃止した。
  // プレビュー別WebView の領域追従（design doc 6章「ネイティブ子WebView が DOM 領域を追う」）。
  // `#preview-host` のサイズ変化（ペイン/差分トグル/分割）と window resize の両方で矩形を backend へ送る。
  // プレビュー非表示中は syncPreviewBounds が無害化する（占有チェック）。
  const previewResizeObserver = new ResizeObserver(() => syncPreviewBounds());
  previewResizeObserver.observe(previewHost());
  window.addEventListener("resize", () => syncPreviewBounds());
  // 外部変更/監視モードの購読（backend の emit を受ける）。
  await onFsChanged((payload) => onExternalChange(payload.changes));
  await onWatchMode((message) => notify(message, "info"));
  // 単一インスタンス転送（要件3.4）: 既存インスタンスへ後続プロセスが投げたパスを開く。
  await onOpenRequest((payload) => void onOpenRequestEvent(payload));
  // 終了直前にアプリ状態を保存（要件10.1。アトミック書込は backend）。デバウンス待ちでなく即時 flush。
  window.addEventListener("beforeunload", () => void persistAppStateNow());
  // 起動時に state.json を復元（version 安全側・復元3分岐は backend が判定）。
  await restoreOnStartup();
  // Empty 3分岐の「フォルダ未オープン（no-folder）」文言（ui-design 15章）。
  if (!state.folder) setStatus(emptyMessage("no-folder"));
}

void main();
