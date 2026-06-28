// pika フロントエンドのエントリ。
// 中心体験①②: フォルダを開く → ツリー → タブで CM6 を開く → 編集 → 保存／外部変更を未読反映。
// 差分/プレビュー/単一インスタンス等は後続スプリントで肉付けする。
import {
  openWorkspace,
  listDir,
  readFile,
  pathKind,
  openDocument,
  reopenDocumentWithEncoding,
  saveDocument,
  f5Resync,
  computeFileDiff,
  confirmFile,
  confirmAll,
  rollbackFile,
  onFsChanged,
  onWatchMode,
  onOpenRequest,
  takeStartupOpenRequest,
  hashContent,
  noteRecent,
  showPreview,
  hidePreview,
  setPreviewBounds,
  syncPreviewScroll,
  logFolderPath,
  openInDefaultApp,
  openLogFolder,
  allowSavePath,
  allowOpenPath,
  getSettings,
  onSettingsWarning,
  onSettingsChanged,
  imageInfo,
  assetUrl,
  type TreeEntry,
  type Settings,
  type OpenRequestPayload,
  type DocEncoding,
  type PreviewRect,
  type ImageInfo,
} from "./ipc";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { open as openNativeDialog, save as saveNativeDialog } from "@tauri-apps/plugin-dialog";
import { initTheme, applyTheme, type ThemeMode } from "./theme";
import { initMenuBar } from "./ui/menu";
import { initA11y, announce } from "./a11y";
import { resolveShortcut, modsOf, normalizeKey, type Action, type Focus } from "./shortcuts";
import { renderTree, resetTreeExpansion } from "./ui/tree";
import { renderTabs } from "./ui/tabs";
import { notify, notices } from "./ui/notifications";
import { setStatus, renderStatus } from "./ui/status";
import { UnreadStore } from "./ui/unread";
import {
  renderImageView,
  renderOpenExternally,
  classifyExtension,
  type ImageFit,
} from "./ui/image";
import { degradeReasonsFromFlags, degradeMessage, emptyMessage } from "./ui/viewstate";
import { createEditor, type EditorHandle } from "./editor";
import { createSearchController, type SearchController } from "./search";
import { renderDiff, type DiffHandle } from "./diff";
import {
  buildPreview,
  resolveOccupancy,
  resolvePreviewTheme,
  PreviewSerializer,
  type ViewMode,
} from "./preview";
import type { PreviewTheme } from "./ipc";
import { pathKey, basename } from "./util/path";
import {
  workbench,
  treeHeadLabel,
  treeCollapseBtn,
  treeExpandBtn,
  editorPane,
  editorHost,
  diffHost,
  previewHost,
  imageHost,
  modeButtons,
  toggleDiffBtn,
  confirmBtn,
  openExternalBtn,
  tabListBtn,
  tabsEl,
  hiddenUnreadBadge,
} from "./ui/dom";
import { promptText, confirmModal, confirmDiscardModal, isModalOpen } from "./ui/modal";
import { openContextMenu, type ContextItem } from "./ui/context-menu";
import { type OpenTab, type AppShellState } from "./app/types";
import { buildMenuSpecs, encodingLabel } from "./ui/menu-specs";
import { createTreeActions } from "./ui/tree-actions";
import { createPersistence } from "./app/persistence";

// OpenTab / AppShellState の型定義は src/app/types.ts へ移した（S8・実体は下の state が持つ）。

const state: AppShellState = {
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
  // 行の折り返し（表示メニューのトグル・UIブラッシュアップ T8）。アプリ全体で 1 つ。
  // タブ切替でエディタを作り直しても createEditor の初期値で引き継ぐ。
  // 既定 ON（ui-design §120 の折り返しトグル準拠）。短い行でも横スクロールバーが常時出る問題を
  // 防ぐため折り返しを既定にし、長い行を見たいときだけ表示メニューで OFF にする運用へ倒す。
  lineWrapping: true,
  // タブの表示幅（settings.toml の tab_width・EditorState.tabSize へ流す）。アプリ全体で 1 つ。
  // lineWrapping と同じく、タブ切替でエディタを作り直しても createEditor の初期値で引き継ぐ。
  // 既定 4（settings 取得前/取得失敗時の素の値）。applySettings が getSettings/settings-changed で更新する。
  tabWidth: 4,
  // settings.toml の有効設定（要件10.3/10.4）。起動時に getSettings で取得し、settings-changed で更新する。
  // 可視4項目（theme/wrap_default/tab_width/default_mode）は applySettings が即適用する（U2a）。
  // 残りの backend 消費分（excluded_dirs/huge_file_threshold/sensitive_patterns/allow_remote_resources/
  // feature_*/full_hash_on_startup）の適用は U2b で別途。null の間は未取得（取得失敗時も含む）。
  settings: null as Settings | null,
};

/**
 * 画像簡易ビューの表示倍率（要件12.2・U3）。アプリ全体で 1 つ。画像タブを開く/切替えるたびに "fit" へ
 * リセットする（前画像で「等倍」にしていても新しい画像はウィンドウフィットで開く＝予期しない巨大表示を避ける）。
 */
let imageFit: ImageFit = "fit";

/**
 * 検索/置換バー（U4/U5・要件5.4）。main() で 1 度だけ生成し、open/close する。
 * deps は getter 経由で常に最新の state.editor / 内容を取るので、タブ切替で editor を作り直しても
 * 正しいエディタへハイライト/置換が向く。
 */
let searchController: SearchController | null = null;

// メインWebView シェルの DOM 要素ゲッタは src/ui/dom.ts へ集約した（S7）。
// 取得対象（#workbench/#tabs/#editor-host …）と null 非保証キャストは不変。

/**
 * アクティブタブで「確認済みにする」が**効果を持つ**か（指摘7・要件8.3）。
 *
 * 確認済み＝外部変更（未読 ±/◆）を再照合してベースラインへ取り込む操作。差分あり（unread の
 * modified/created）でないファイルは確認するものが無く、押しても no-op なので始めから無効化する。
 * 削除済み（ディスク実体なし・rollback 導線が別）・非テキスト（画像/バイナリ＝確認の概念なし）・
 * 新規未保存（unread が付かない＝disk 実体なし）も対象外。
 */
function activeCanConfirm(): boolean {
  if (!state.active) return false;
  const tab = state.tabs.find((t) => t.path === state.active);
  if (!tab || tab.deleted || tab.nonText) return false;
  const kind = state.unread.get(state.active);
  return kind === "modified" || kind === "created";
}

function refreshTabs(): void {
  // onClose は (path)=>void 型。closeTab は async（Promise を返す）なので void で包んで Promise を捨てる。
  renderTabs(
    state.tabs,
    state.active,
    activateTab,
    state.unread,
    (p) => void closeTab(p),
    (p, x, y) =>
      openContextMenu([{ label: "パスをコピー", run: () => void copyPathToClipboard(p) }], x, y),
  );
  // アクティブタブが横スクロール域の外（新規に末尾へ追加された等）なら可視域へ寄せる（指摘2 補強）。
  // min-width:0（#editor-pane）で overflow-x が正しく効くようになった上で、切替/追加時に確実に見せる。
  scrollActiveTabIntoView();
  // tab-tools（モード切替セグメント/差分トグル）の表示状態・有効/無効を同期する。
  refreshViewTools();
  // 「確認済みにする」は tab-tools 右端に残るボタン（T6）。退避＋ベースライン更新を伴うため
  // in-flight 中（busy）は無効に保つ（内部 refreshTabs で再有効化して二重送信を許さない）。
  // 保存/すべて確認済み/巻き戻しはツールバー廃止（T8）でメニューへ移したため、活性はメニューを
  // 開いた時点で都度算出する（ui/menu.ts の build が hasActive/busy を反映）。ここでは扱わない。
  // 効果のないボタンは始めから無効化する（指摘7）: 差分あり（未読 modified/created）でないファイルは
  // 確認するものが無い＝確認済みボタンを無効にする（新規未保存/削除済み/非テキストも対象外）。
  confirmBtn().disabled = !activeCanConfirm() || state.busy;
  // タブ描画が変わると可視範囲も変わるので隠れ未読バッジを再計算する（更新タイミング(b)）。
  scheduleHiddenUnread();
}

/**
 * 隠れた差分あり（未読）タブ数のバッジ更新を rAF でまとめる（T10・差分 C5）。
 * scroll/resize/再描画/レイアウト変化のたびに呼ばれるので、同一フレーム内の連続呼び出しを
 * 1 回の計測へ畳んでレイアウトスラッシング（読み書き交互）と過剰計算を避ける（制約: 計算は軽量に）。
 */
let hiddenUnreadRaf = 0;
function scheduleHiddenUnread(): void {
  if (hiddenUnreadRaf) return; // 既にこのフレームの計測を予約済み。
  hiddenUnreadRaf = requestAnimationFrame(() => {
    hiddenUnreadRaf = 0;
    updateHiddenUnread();
  });
}

/**
 * 横スクロールで画面外に隠れた「差分あり（未読）」タブ数を数え、「▸ 未読N」バッジへ反映する
 * （要件5.3「隠れた差分ありタブはタブバー端のバッジで示す」・ui-mock .hidden-unread）。
 *
 * 隠れ判定: #tabs（overflow-x:auto）の可視窓 [scrollLeft, scrollLeft+clientWidth] に対し、
 * 各タブ要素の [offsetLeft, offsetLeft+offsetWidth] が **完全にはみ出して見えていない**（左へ
 * スクロールアウト or 右へスクロールアウト）ものを隠れタブとする。一部でも窓に重なれば可視扱い。
 * そのうち未読（unread.get(path) が modified/created）のものを数える。削除済み(removed)は対象外
 * （差分あり＝modified/created。ui-design 5章の状態記号と一致）。
 *
 * 計測は読み取りのみを連続で行い（書き込みは最後の DOM 反映 1 回）、スラッシングを避ける。
 */
function updateHiddenUnread(): void {
  const badge = hiddenUnreadBadge();
  const container = tabsEl();
  // ---- 読み取りフェーズ（レイアウトプロパティをまとめて読む）----
  const viewLeft = container.scrollLeft;
  const viewRight = viewLeft + container.clientWidth;
  const tabEls = Array.from(container.querySelectorAll<HTMLElement>('[role="tab"]'));
  let hiddenUnread = 0;
  let firstHiddenUnread: HTMLElement | null = null;
  for (const el of tabEls) {
    const path = el.dataset.path;
    if (!path) continue;
    const kind = state.unread.get(path);
    if (kind !== "modified" && kind !== "created") continue; // 差分あり（未読）のみ対象。
    const left = el.offsetLeft;
    const right = left + el.offsetWidth;
    // 可視窓と全く重ならない（右端 <= 窓左 or 左端 >= 窓右）＝隠れている。1px の丸めは許容。
    const hidden = right <= viewLeft + 1 || left >= viewRight - 1;
    if (hidden) {
      hiddenUnread += 1;
      if (!firstHiddenUnread) firstHiddenUnread = el;
    }
  }
  // クリックで最初の隠れ未読タブへスクロールするための参照を保持（親切機能・任意）。
  hiddenUnreadTarget = firstHiddenUnread;
  // ---- 書き込みフェーズ（DOM 反映は 1 回だけ）----
  if (hiddenUnread > 0) {
    badge.textContent = `▸ 未読${hiddenUnread}`;
    badge.setAttribute("aria-label", `画面外に差分ありのタブが ${hiddenUnread} 件`);
    badge.hidden = false;
  } else {
    badge.hidden = true;
    badge.removeAttribute("aria-label");
    hiddenUnreadTarget = null;
  }
}

/**
 * アクティブタブが横スクロール域の外なら可視域へ寄せる（指摘2 補強）。
 * 末尾へ新規追加されたタブ/キーボード移動でアクティブが画面外になったとき、見切れたまま放置しない。
 * 横方向のみを自前計算してずらす（タブ列は横スクロール専用・縦スクロールを誘発しない）。
 */
function scrollActiveTabIntoView(): void {
  const container = tabsEl();
  const active = container.querySelector<HTMLElement>('[role="tab"][aria-selected="true"]');
  if (!active) return;
  const left = active.offsetLeft;
  const right = left + active.offsetWidth;
  const viewLeft = container.scrollLeft;
  const viewRight = viewLeft + container.clientWidth;
  if (left < viewLeft) {
    container.scrollLeft = Math.max(0, left - 12);
  } else if (right > viewRight) {
    container.scrollLeft = right - container.clientWidth + 12;
  }
}

/** バッジクリックでスクロールする先（最初の隠れ未読タブ要素）。updateHiddenUnread が更新する。 */
let hiddenUnreadTarget: HTMLElement | null = null;

/**
 * 隠れ未読バッジのクリックで、最初の隠れ未読タブが見える位置まで #tabs を横スクロールする
 * （T10 親切機能・任意）。scrollIntoView は縦スクロールも誘発しうるので、横方向のみを
 * 自前で計算してずらす（タブ列は横スクロール専用）。スクロール後の scroll イベントで
 * scheduleHiddenUnread が走り、バッジは自然に再計算される。
 */
function scrollToHiddenUnread(): void {
  const target = hiddenUnreadTarget;
  if (!target) return;
  // 対象タブが可視窓の左端に来るよう寄せる（少し余白を残して窓内に確実に収める）。
  tabsEl().scrollTo({ left: Math.max(0, target.offsetLeft - 12), behavior: "smooth" });
}

/**
 * tab-tools（モード切替セグメント＋差分トグル）の表示状態を現在の state へ同期する
 * （UIブラッシュアップ T6 差分 C4・ui-design 8章）。
 *
 * - モードセグメント: 現在の state.viewMode のボタンに .on＋aria-pressed=true を付ける（排他）。
 * - 差分トグル: state.diffOn のとき .on（accent-strong 塗り）＋aria-pressed=true。
 * - 有効/無効: アクティブタブが無い間はモード/差分とも無効（中心体験の起点が無いため）。
 *
 * setViewMode/onToggleDiff/applyOccupancy・ショートカット発火後にこれを呼んで UI を一致させる。
 */
function refreshViewTools(): void {
  const hasActive = !!state.active;
  // 非テキスト（画像/非対応バイナリ＝要件12.2・U3）にモード/差分は無いので無効化する。
  const activeTab = state.tabs.find((t) => t.path === state.active);
  const nonText = !!activeTab && activeTab.nonText && !activeTab.deleted;
  for (const btn of modeButtons()) {
    const on = btn.dataset.mode === state.viewMode;
    btn.classList.toggle("on", on);
    btn.setAttribute("aria-pressed", String(on));
    btn.disabled = !hasActive || nonText;
  }
  const diffBtn = toggleDiffBtn();
  diffBtn.disabled = !hasActive || nonText;
  diffBtn.classList.toggle("on", state.diffOn);
  diffBtn.setAttribute("aria-pressed", String(state.diffOn));
  // 「ブラウザで開く」はアクティブタブのファイルが対象。画像も既定アプリで開けるので活性のまま。
  // アクティブが無い間のみ無効にする（誤クリック防止）。
  openExternalBtn().disabled = !hasActive;
  // 全タブ一覧ドロップダウンはタブが 1 枚以上あるとき有効（無いと出すものが無い）。
  tabListBtn().disabled = state.tabs.length === 0;
}

/**
 * 全タブ一覧ドロップダウンを開く（要件5.3「開いているタブを一覧から選ぶ」）。
 *
 * 各項目に状態マーク（差分あり ±／新規 ◆／削除済み／未保存 ●）とアクティブ印（✓）を付け、選択で
 * そのタブへ切り替える（activateTab）。キーボード操作（↑/↓/Enter）対応で開く（openContextMenu の
 * focusFirst）。タブ列が横スクロールで隠れていても全タブへ到達できる導線になる。
 */
function showAllTabsMenu(anchor: HTMLElement): void {
  if (state.tabs.length === 0) return;
  const items: ContextItem[] = state.tabs.map((t) => {
    const marks: string[] = [];
    const kind = state.unread.get(t.path);
    if (kind === "removed") marks.push("削除済み");
    else if (kind === "created") marks.push("◆");
    else if (kind === "modified") marks.push("±");
    if (t.dirty) marks.push("●");
    const prefix = marks.length > 0 ? `${marks.join(" ")} ` : "";
    return {
      label: `${prefix}${t.title}`,
      checked: t.path === state.active, // アクティブタブに ✓。
      run: () => void activateTab(t.path),
    };
  });
  const rect = anchor.getBoundingClientRect();
  openContextMenu(items, rect.left, rect.bottom, { focusFirst: true });
}

function refreshTree(): void {
  // 子フォルダの遅延展開は副作用なし列挙（listDir）で取得する（監視ルート付け替え/ベースライン再取得をしない）。
  // 右クリックで新規ファイル/新規フォルダ/削除のコンテキストメニューを出す（要件11・T5）。
  renderTree(
    state.treeEntries,
    (entry) => void openFile(entry),
    listDir,
    state.unread,
    (entry, x, y) => showTreeContextMenu(entry, x, y),
  );
}

/**
 * データ変更（確認/一括確認/巻き戻し）後の標準再描画クラスタ（S7・重複統合）。
 * ツリー/タブ/構造化ステータスを更新し、差分トグル ON のときは新ベースライン基準で差分を貼り直す。
 * 旧 onConfirm/onConfirmAll/onRollback で同一だった4行（refreshTree→refreshTabs→refreshStatus→差分再描画）の単一源。
 */
async function refreshAll(): Promise<void> {
  refreshTree();
  refreshTabs();
  refreshStatus(); // 差分あり数（差分 N）を更新。
  if (state.diffOn) await renderActiveDiff();
}

// ── ツリーのコンテキストメニュー＋新規作成/削除（要件11・design G・T5）─────────────────────
// コンテキストメニューの土台（openContextMenu/closeContextMenu）は src/ui/context-menu.ts へ、
// 自前モーダル（promptText/confirmModal/confirmDiscardModal）とグローバルショートカット抑止判定
// （isModalOpen＝旧 modalDepth）は src/ui/modal.ts へ集約した（S7・挙動不変）。

/**
 * ツリーから今しがた自分で作成したパス→登録時刻(ms)（指摘4）。watcher は workspace を再帰監視するため、
 * pika 内で作った空ファイルにも created イベントを emit する。これを未読(◆ 新規)として扱うと「たった今
 * 自分で作ったファイル」が未読・確認対象に見えてしまう。created イベント到着時にこの Map で**ワンショット
 * 抑制**（非同期到着と競合しないよう到着時点で判定・消費）。キーは selfKey で正規化する。
 *
 * 値に登録時刻を持たせ TTL で stale エントリを掃除する（第2巡 回帰修正）。watcher が当該 created を
 * 合体/取りこぼしで一度も emit しないと未消費エントリが恒久的に残り、後で外部ツールが同パスを再作成した
 * ときその正当な外部 created を握りつぶす（過剰一致）＋ Set 無制限増加が起きるため。
 */
const selfCreatedPaths = new Map<string, number>();

/** 自作成抑制エントリの寿命（ms）。これを過ぎた未消費分は次回 onExternalChange で破棄する。 */
const SELF_CREATED_TTL_MS = 15000;

/** パスを selfCreatedPaths のキーへ正規化する（区切り `\`→`/`・Windows の大小無視）。 */
function selfKey(p: string): string {
  // pathKey（区切り正規化・大小保持）に小文字化を重ねる＝旧 `p.replace(/\\/g,"/").toLowerCase()` と同一。
  return pathKey(p).toLowerCase();
}

/** TTL を過ぎた未消費の自作成抑制エントリを破棄する（stale 過剰一致＋無制限増加の防止・第2巡）。 */
function pruneSelfCreated(now: number): void {
  for (const [k, t] of selfCreatedPaths) {
    if (now - t > SELF_CREATED_TTL_MS) selfCreatedPaths.delete(k);
  }
}

/**
 * 自作成パスを watcher の created 未読抑制へ登録する（指摘4・tree-actions から注入）。
 * 登録は onExternalChange（main 在籍）が onExternalChange 到着時にワンショット消費する。
 */
function markSelfCreated(path: string): void {
  selfCreatedPaths.set(selfKey(path), Date.now());
}

// ツリーのコンテキストメニュー（showTreeContextMenu/showRootContextMenu）と新規作成/削除/階層更新は
// src/ui/tree-actions.ts へ抽出した（S8・挙動不変）。下の DI 結線で createTreeActions に注入する。

/** タブ右クリックの「パスをコピー」。フルパスをクリップボードへ書き、結果を通知する。 */
async function copyPathToClipboard(path: string): Promise<void> {
  const ok = await copyText(path);
  notify(ok ? "パスをコピーしました" : "パスのコピーに失敗しました", ok ? "info" : "error");
}

/**
 * テキストをクリップボードへコピーする。クリップボード専用プラグインは入れず（軽量優先）、
 * まず navigator.clipboard.writeText（ユーザー操作＝右クリック起点なので WebView2 でも許可される）、
 * 失敗時は一時 textarea + execCommand("copy") へフォールバックする。成否を boolean で返す。
 */
async function copyText(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch {
    // フォールバックへ。
  }
  try {
    const ta = document.createElement("textarea");
    ta.value = text;
    ta.style.position = "fixed";
    ta.style.opacity = "0";
    ta.style.pointerEvents = "none";
    document.body.appendChild(ta);
    ta.select();
    const ok = document.execCommand("copy");
    document.body.removeChild(ta);
    return ok;
  } catch {
    return false;
  }
}

// パス操作（parentDir/samePath/basename/pathKey）は src/util/path.ts へ集約した（S7・挙動不変）。

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
  // 末尾区切りは除去済みなので basename（`?? trimmed`）と旧 `|| trimmed` は同値（pop が "" を返すのは
  // trimmed が空のときだけで、その場合は両者とも ""）。
  const trimmed = folder.replace(/[\\/]+$/, "");
  const base = basename(trimmed);
  label.textContent = `エクスプローラー — ${base}`;
  // 省略表示でも全パスが分かるよう title（ツールチップ）に絶対パスを残す。
  label.setAttribute("title", folder);
}

/**
 * フレームレス化（tauri.conf decorations:false・ui-design §7）に伴う自前ウィンドウ操作ボタンを配線する。
 * OS タイトルバーを廃した分、最小化/最大化(復元)/閉じるを @tauri-apps/api/window 経由で実装する。
 * ドラッグ移動はメニュー帯の data-tauri-drag-region（index.html）が担う。リサイズはフレームレスでも
 * Tauri/WebView2 がウィンドウ枠のヒットテストを保持する（decorations:false でも resizable:true は有効）。
 * 最大化トグルは現在状態を反映してアイコン/ラベルを「最大化⇔元のサイズに戻す」へ切替える。
 */
function initWindowControls(): void {
  const minBtn = document.getElementById("win-minimize") as HTMLButtonElement | null;
  const maxBtn = document.getElementById("win-maximize") as HTMLButtonElement | null;
  const closeBtn = document.getElementById("win-close") as HTMLButtonElement | null;
  // ブラウザ単体（vite preview 等・Tauri 非依存）でも型/起動が壊れないよう存在チェックして握る。
  if (!minBtn || !maxBtn || !closeBtn) return;
  const win = getCurrentWindow();
  minBtn.addEventListener("click", () => void win.minimize());
  closeBtn.addEventListener("click", () => void win.close());
  // 最大化トグル（最大化⇔復元）。押下後に現在状態をアイコン/ラベルへ反映する。
  const syncMaxLabel = async (): Promise<void> => {
    const maximized = await win.isMaximized();
    maxBtn.setAttribute("aria-label", maximized ? "元のサイズに戻す" : "最大化");
    maxBtn.setAttribute("title", maximized ? "元のサイズに戻す" : "最大化");
    maxBtn.classList.toggle("is-maximized", maximized);
    // 最大化中は上端リサイズ縁（#resize-top）を CSS で消す（指摘8）。最大化したウィンドウは上辺リサイズが
    // 無効なのに ns-resize カーソルが出て「大きさ調整できそう」に見える誤解を防ぐ（通常カーソルへ戻す）。
    document.body.classList.toggle("window-maximized", maximized);
  };
  maxBtn.addEventListener("click", () => {
    void (async () => {
      await win.toggleMaximize();
      await syncMaxLabel();
    })();
  });
  // 起動時・OS 側操作（ダブルクリック/スナップ）での最大化変化にも追従させる。
  void syncMaxLabel();
  void win.onResized(() => void syncMaxLabel());
  // フレームレス上端のリサイズハンドル（decorations:false で OS の上辺リサイズ縁が細いのを補強・
  // ui-design §7）。上端の薄いストリップ上で mousedown したら上辺リサイズを Tauri へ依頼する
  // （capabilities: core:window:allow-start-resize-dragging）。最大化中は何もしない。
  const resizeTop = document.getElementById("resize-top");
  resizeTop?.addEventListener("mousedown", (e) => {
    if (e.button !== 0) return;
    void (async () => {
      if (await win.isMaximized()) return;
      // ResizeDirection は @tauri-apps/api/window 内の文字列ユニオン型（未 export）。文字列リテラルで渡す。
      await win.startResizeDragging("North");
    })();
  });
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
  // ツリー収納/引き出しでタブ列の幅が変わる＝可視タブ範囲が変わるので隠れ未読バッジを再計算する
  // （更新タイミング(d): レイアウト変化時）。scheduleHiddenUnread が rAF でまとめる。
  scheduleHiddenUnread();
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

/**
 * 退避を伴う操作の in-flight 抑止（withBusy 用）。
 * ツールバー廃止（T8）で保存/すべて確認済み/巻き戻しはメニューへ移り、メニューは開いた時点で
 * busy を反映するため常設ボタンは tab-tools の「確認済みにする」のみ。busy フラグ自体が
 * メニュー項目（build）と dispatchAction の二重送信を弾く一次ガードになる。
 */
function setMutatingButtonsDisabled(disabled: boolean): void {
  confirmBtn().disabled = disabled;
}

async function activateTab(path: string): Promise<void> {
  // タブ切替はエディタを destroy→再生成するため、検索バーのハイライト（StateField）は破棄される。
  // 古いエディタ向けの検索状態を残さないようバーを閉じる（U4・要件5.4）。
  searchController?.close();
  // 切替前に現在タブの位置を退避しておく（後でこのタブへ戻ったとき位置を復元するため）。
  captureActivePosition();
  state.active = path;
  // 通知バーのタブ固有通知をこのタブへ切り替える（要件11.1: タブ切替で表示が切り替わる）。
  notices.setActiveTab(path);
  const tab = state.tabs.find((t) => t.path === path);
  // 外部リソース許可は **タブ切替で必ず既定オフに戻す**（要件6.2「既定は必ずオフに戻る」・永続しない）。
  // 「許可して再読込」はこのタブをアクティブに見ている間だけ有効で、切り替えると遮断へ戻る。
  if (tab) tab.allowExternal = undefined;
  // 非テキスト（画像/非対応バイナリ＝要件12.2・U3）は CM6 を作らず image-host で表示する。
  // 削除済みでない非テキストタブのみここで処理する（削除済みは下の deleted 分岐＝rollback 導線を優先）。
  if (tab && tab.nonText && !tab.deleted) {
    // 新しい画像はウィンドウフィットで開く（前画像の「等倍」を引きずらない）。
    imageFit = "fit";
    // CM6 を破棄して editor を null にする（captureActivePosition/refreshStatus/captureTabHash は
    // !state.editor で早期 return するため、画像タブ中はこれらが安全に no-op になる）。
    state.editor?.destroy();
    state.editor = null;
    refreshTabs();
    await renderNonTextTab(tab);
    // host 可視を正規化する（image-host のみ表示・preview 子WebView は隠す）。
    applyOccupancy();
    void persistAppState();
    return;
  }
  // 削除済みタブはディスクから読めない。退避/ベースラインは snapshot に残るので、
  // 直近内容を空にしてエディタを出し「確認済み時点に戻す（rollback）」導線を保つ（eval high）。
  // ただし削除済みタブを編集して別タブへ切り替えていた場合（dirty かつ draft あり）は、その未保存編集を
  // 失わないよう draft を載せ直す（eval high #11・データを失わない）。
  if (tab?.deleted) {
    const deletedContent = tab.dirty && tab.draft !== undefined ? tab.draft : "";
    state.editor?.destroy();
    state.editor = createEditor({
      parent: editorHost(),
      initialDoc: deletedContent,
      onChange: () => markDirty(path),
      onCursorChange: () => refreshStatus(),
      lineWrapping: state.lineWrapping,
      tabWidth: state.tabWidth,
      // 削除済みタブの内容は draft/空＝小さいが、直近の段階制（editing_off）を踏襲して重い装飾の要否を決める。
      heavy: !tab.editingOff,
      // エディタ→プレビュー片方向スクロール同期（S4・要件6.1 改訂）。
      onScroll: onEditorScroll,
      // 言語選択（拡張子で markdown/HTML を切替・要件5.1）。
      filePath: path,
    });
    refreshTabs();
    // 直前に画像タブを見ていた場合に image-host が残らないよう隠す（画像→削除済みタブ切替の正規化）。
    imageHost().hidden = true;
    setStatus(`削除済み: ${path}（［確認済み時点に戻す］で退避から復元できます）`);
    return;
  }
  try {
    // open_document でエンコーディングを判定して開く（保存時に元エンコーディングを維持する＝要件5.2）。
    // 第2段階以降は text が空（仮想化ビューアは系統C）。通常/第1段階のテキストをエディタへ載せる。
    const doc = await openDocument(path);
    // 最新性チェック（eval high #10）: await 中に別タブへ切り替わっていたら以後の destroy/create を
    // 一切行わずに抜ける。これをしないと、後着の openDocument 結果で現アクティブタブのエディタを
    // 別パスの内容に作り替えてしまい、「アクティブタブと実エディタ内容がずれ」別タブ内容を別パスへ
    // 保存しうる（最上位原則「データを失わない」）。古い起動は静かに破棄して構わない。
    if (state.active !== path) return;
    if (tab) {
      // 検出エンコーディング/BOM/改行コードをタブに保持し、保存時 save_document へ渡す
      //（暗黙 UTF-8 化の防止・eval medium）。改行コードは表示メニューの現在値表示にも使う（要件5.2）。
      tab.encoding = doc.encoding;
      tab.hasBom = doc.has_bom;
      tab.lineEnding = doc.line_ending;
      tab.editingOff = doc.degrade.editing_off;
    }
    // 未保存編集を持つタブ（dirty かつ draft あり）は **ディスク内容で上書きしない**（eval high #11・
    // データを失わない）。退避しておいた編集中テキスト（draft）を CM6 へ載せ直し、未保存状態を保つ。
    // それ以外（非 dirty）は openDocument が読んだディスク/段階制テキストを正として載せる。
    const hasDraft = tab?.dirty && tab.draft !== undefined;
    const content = hasDraft ? (tab as OpenTab).draft! : doc.text;
    state.editor?.destroy();
    state.editor = createEditor({
      parent: editorHost(),
      initialDoc: content,
      onChange: () => markDirty(path),
      onCursorChange: () => refreshStatus(),
      lineWrapping: state.lineWrapping,
      tabWidth: state.tabWidth,
      // 巨大ファイル段階制（highlight_off/editing_off）が立つときは重い装飾を外す（heavyDecoCompartment）。
      heavy: !(doc.degrade.highlight_off || doc.degrade.editing_off),
      // エディタ→プレビュー片方向スクロール同期（S4・要件6.1 改訂）。
      onScroll: onEditorScroll,
      // 言語選択（拡張子で markdown/HTML を切替・要件5.1）。
      filePath: path,
    });
    // host 可視を正規化する（画像タブ→テキストタブ切替で image-host が残らないよう必ず隠す＝U3）。
    // applyOccupancy は hidden 切替＋bounds のみで preview ナビゲートはしないため、下の
    // renderActiveDiff/renderActivePreview（実ナビゲート）の前に置いても順序を壊さない。
    applyOccupancy();
    // 段階制で機能が縮退するとき（preview/diff/highlight 等の自動オフ）は理由を通知バー提示する（要件2.2）。
    notifyDegrade(path, doc.degrade);
    if (doc.decode_warning) {
      notify(`エンコーディングを自動判定できず UTF-8 で開きました: ${tab?.title ?? path}`, "warn");
    }
    // 開いた内容の実ハッシュを記録する（復元の別物=未読判定に使う・eval high）。
    // draft（未保存編集）を載せたときはディスクと異なるので記録しない（contentHash はディスク/保存内容
    // を表す素のままにする＝復元の別物判定を歪めない）。非 dirty 時のみディスク内容のハッシュを記録する。
    if (!hasDraft) void captureTabHash(path, content);
    // タブに保持した位置（復元/前回アクティブ時）へカーソル/スクロールを戻す（要件10.1）。
    if (tab && (tab.cursorLine > 1 || tab.cursorColumn > 1 || tab.scrollTop > 1)) {
      state.editor.gotoPosition(tab.cursorLine, tab.cursorColumn);
      state.editor.scrollToLine(tab.scrollTop);
    }
    refreshTabs();
    // 開いた直後の構造化ステータス（差分あり数・行数・文字数・カーソル位置）を描画する（D2/D3）。
    refreshStatus();
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

/**
 * 非テキストタブ（画像/非対応バイナリ＝要件12.2・U3）を image-host へ描画する。
 *
 * backend の image_info で種別を確かめ、画像なら assetUrl の custom protocol 経由で <img> 表示
 * （ウィンドウフィット/等倍トグル付き）、巨大画像/非対応/開けない（機密拒否等）は「既定アプリで開く」
 * 誘導を出して行き止まりにしない（要件12 縮退時の next-action）。通知は出しすぎない（描画自体が状態提示）。
 *
 * 画像の内容ハッシュ baseline（復元時の別物検知）は MVP 外＝未対応。live 変更検知は watcher の
 * パスイベント（fs-changed の未読）で満たされ、画像タブも特別な結線なしにそれへ乗る（要件12.2）。
 */
async function renderNonTextTab(tab: OpenTab): Promise<void> {
  const host = imageHost();
  host.replaceChildren();
  host.hidden = false;
  try {
    const info: ImageInfo = await imageInfo(tab.path);
    // await 中に別タブへ切り替わっていたら描画を破棄する（最新性チェック・後着結果で別タブを汚さない）。
    if (state.active !== tab.path) return;
    if (info.kind === "image") {
      // fit/actual トグル付きで描画する。トグルは imageFit を更新して同じ画像を再描画する
      // （コールバックを再生成するため draw を自己参照する）。
      const draw = (): void =>
        renderImageView(imageHost(), assetUrl(tab.path), imageFit, (f) => {
          imageFit = f;
          draw();
        });
      draw();
    } else if (info.kind === "too-large") {
      renderOpenExternally(
        host,
        "画像が大きすぎるため簡易ビューでは表示できません（既定のアプリで開いてください）",
        () => void onOpenExternal(),
      );
    } else {
      renderOpenExternally(
        host,
        "対応していない形式です（既定のアプリで開いてください）",
        () => void onOpenExternal(),
      );
    }
  } catch (e) {
    // image_info が Err を返すケース（機密ファイル拒否・I/O 失敗等）。行き止まりにせず外部誘導を出す。
    if (state.active !== tab.path) return;
    renderOpenExternally(host, `開けませんでした: ${String(e)}`, () => void onOpenExternal());
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

/**
 * 右下の構造化ステータス（ui-mock .status）を現在状態から組み立てて描画する（D2/D3・要件11.1）。
 *
 * 内容＝差分あり（未読）数＋全体「行数・文字数」＋カーソル位置「行・文字目」。全体は text-2、
 * カーソル位置は accent で色分けする（CSS）。プレビュー/差分などカーソルが無いモードでは
 * 全体（差分/行/文字）のみ出し、カーソル位置は出さない（ui-design 9章）。
 *
 * アクティブタブが無い（フォルダ未オープン/全タブを閉じた）ときは状況メッセージ（setStatus）に委ね、
 * ここでは何もしない（呼び出し側が setStatus を出している）。
 */
function refreshStatus(): void {
  if (!state.active || !state.editor) return;
  const unreadCount = state.unread.unreadCount();
  // カーソルを持つのはソース編集中のみ（エディタが占有しているとき）。プレビュー/差分占有時は全体のみ。
  const occ = resolveOccupancy(state.viewMode, state.diffOn);
  if (occ.showEditor) {
    const m = state.editor.getMetrics();
    renderStatus({
      unreadCount,
      lines: m.lines,
      chars: m.chars,
      cursorLine: m.cursorLine,
      cursorColumn: m.cursorColumn,
      selectionChars: m.selectionChars,
    });
  } else {
    // プレビュー/差分占有中はカーソルが無いので全体（差分・行・文字）のみ。行/文字はバッファ計測値を流用。
    const m = state.editor.getMetrics();
    renderStatus({ unreadCount, lines: m.lines, chars: m.chars });
  }
}

/**
 * アクティブエディタのカーソル/スクロール（と dirty 時の編集中テキスト）を現在タブへ写す。
 * 保存前・タブ切替前（activateTab 冒頭）・状態保存前（collectAppState）に呼ぶ（要件10.1）。
 *
 * eval high #11: dirty なタブから切り替える瞬間に編集中テキストを tab.draft へ退避する。これにより
 * 単一 CM6 を共有していても未保存編集がタブ切替で失われない（再アクティブ化で draft を載せ直す）。
 * 非 dirty なら draft は持たない（ディスク/ベースラインが正なので退避不要）。
 */
function captureActivePosition(): void {
  if (!state.active || !state.editor) return;
  const tab = state.tabs.find((t) => t.path === state.active);
  if (!tab) return;
  const cur = state.editor.getCursor();
  tab.cursorLine = cur.line;
  tab.cursorColumn = cur.column;
  tab.scrollTop = state.editor.getScrollTop();
  // 未保存編集があるタブのみ編集中テキストを退避する（データを失わない）。
  if (tab.dirty) {
    tab.draft = state.editor.getContent();
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
  // 非テキスト（画像/非対応バイナリ）は CM6 へテキスト全量ロードせず簡易ビュー扱いにする（要件12.2・U3）。
  // 拡張子で種別判定し、image も unsupported も「非テキスト」＝nonText=true として CM6 を作らない
  // （実画像配信・寸法プリチェックは backend の image_info / custom protocol が担う＝activateTab で結線）。
  const kind = classifyExtension(entry.name);
  if (!state.tabs.some((t) => t.path === entry.path)) {
    state.tabs.push(newTab(entry.path, entry.name));
  }
  const tab = state.tabs.find((t) => t.path === entry.path);
  if (tab) tab.nonText = kind !== "text";
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
    nonText: false,
    editingOff: false,
    // 既定は BOM なし UTF-8（新規ファイル/不明時）。既存ファイルは open 時に open_document の判定で上書きする。
    encoding: "utf-8",
    hasBom: false,
    // 改行コードは open 時に open_document の判定で上書きする（新規/不明時は none）。表示専用（要件5.2）。
    lineEnding: "none",
  };
}

async function onOpenFolder(): Promise<void> {
  // OS ネイティブ選択ダイアログでフォルダを選ぶ（要件3.2・dialog:allow-open）。
  // 現在フォルダを既定値として提示し、キャンセル（null）時は何もしない。
  // 返却パスは必ず switchFolder 経由で open_workspace に渡す（AccessControl で core 再検証）。
  const picked = await pickNativePath({
    directory: true,
    defaultPath: state.folder ?? undefined,
    // dev ブラウザ単体（Tauri 非依存・open 不在）では prompt にフォールバックする。
    promptMessage: "開くフォルダのパスを入力してください（例 C:\\work\\notes）",
  });
  if (picked === null) return; // キャンセル＝何もしない。
  const trimmed = picked.trim();
  if (!trimmed) {
    notify("フォルダのパスを入力してください", "warn");
    return;
  }
  await switchFolder(trimmed);
}

/**
 * OS ネイティブ選択ダイアログでパスを選ぶ薄いラッパ（要件3.2/11.2）。
 * dev ブラウザ単体（Tauri 非依存・dialog plugin の open が使えない環境）では従来 window.prompt に
 * フォールバックして実機外でも壊さない（存在しない/失敗時はプロンプトへ退避）。
 * 返り値: 選択パス（string）／キャンセル時は null。返却パスの FS 読取は呼び出し側が
 * switchFolder / openPath（AccessControl 再検証経由）で行う＝ここでは直接 FS を触らない。
 */
async function pickNativePath(opts: {
  directory: boolean;
  defaultPath?: string;
  promptMessage: string;
}): Promise<string | null> {
  try {
    // ファイル選択は multiple:false で string|null、フォルダ選択は directory:true で string|null。
    const result = await openNativeDialog({
      directory: opts.directory,
      multiple: false,
      defaultPath: opts.defaultPath,
    });
    // 単一選択なので戻りは string | null（配列にはならない）。配列で来た場合も先頭を採る防御。
    if (result === null) return null; // キャンセル。
    return Array.isArray(result) ? (result[0] ?? null) : result;
  } catch {
    // dialog plugin 不在/失敗（dev ブラウザ単体等）。従来の手入力プロンプトへ退避する。
    const p = window.prompt(opts.promptMessage, state.folder ?? "");
    return p; // null=キャンセル、文字列=入力値（trim は呼び出し側）。
  }
}

/**
 * 未保存タブがある状態での破壊的操作（フォルダ切替）の三択確認（要件5.3・eval medium）。
 * 対象ファイル名を明示し、保存して切替/破棄して切替/キャンセル を選べる。
 *
 * 確認は自前の三択モーダル（confirmDiscardModal）で出す。window.confirm はこの Tauri/WebView2 ビルドで
 * ダイアログを出さず即 true を返すため、旧実装の「2 段にして三択」は常に save へ倒れて無断でデータを失っていた。
 * 1 枚のモーダルに 保存して切替（既定）／破棄して切替（danger）／キャンセル を並べて明示確認する。
 */
function confirmDiscardUnsaved(names: string[]): Promise<"save" | "discard" | "cancel"> {
  // 対象名を列挙（多すぎる場合は先頭数件＋残数）。
  const shown = names.slice(0, 5).join("、");
  const more = names.length > 5 ? ` ほか ${names.length - 5} 件` : "";
  const list = `${shown}${more}`;
  return confirmDiscardModal(
    `未保存の変更があります（${list}）。\nフォルダを切り替える前にどうしますか？`,
  );
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
      // フォルダ切替で前フォルダのエディタを畳むので検索バーを閉じる（古いハイライトを残さない）。
      searchController?.close();
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
    // フォルダ名＋件数はツリーヘッダ（T3）へ移したのでステータスからは外す。ファイルを開くまでは
    // 表示するファイル計測値が無いのでステータスは空にする（開くと refreshStatus が構造化表示する）。
    setStatus("");
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
 * 表現不能文字（unmappable）で保存中断したときの「UTF-8（BOM なし）で保存しますか？」確認（要件5.6・S7 統合）。
 *
 * saveOnce（通常保存）と onSaveAs（名前を付けて保存）で同一だった確認モーダルの単一源。この時点でディスクは
 * 未変更＝データは失われていない。window.confirm はこの Tauri/WebView2 ビルドでダイアログを出さず即 true を
 * 返すため、素通しで無断 UTF-8 変換しないよう自前モーダル（confirmModal）で明示確認する。OK=true で UTF-8 保存へ。
 */
function confirmUtf8Fallback(filePath: string, unmappableCount: number): Promise<boolean> {
  const name = basename(filePath);
  return confirmModal(
    `「${name}」には現在のエンコーディングで保存できない文字が ${unmappableCount} 件あります。\n` +
      `UTF-8（BOM なし）で保存しますか？［キャンセル］を選ぶと保存しません（変更は失われません）。`,
    { okLabel: "UTF-8で保存" },
  );
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
      // ［UTF-8で保存／キャンセル］を確認し、選べば UTF-8（BOM なし）で保存し直す（確認は confirmUtf8Fallback に集約）。
      const ok = await confirmUtf8Fallback(path, result.unmappable.length);
      if (ok) {
        await saveOnce(path, content, tab, true);
      } else {
        notify("保存を中止しました（表現不能文字のため・変更は保持しています）", "warn");
      }
      return;
    }
    if (tab) {
      tab.dirty = false;
      // 保存できたので未保存編集の退避バッファは不要（以後はディスク内容が正・eval high #11）。
      tab.draft = undefined;
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
    refreshStatus(); // 差分あり数（差分 N）を更新。
    notify("保存しました");
  } catch (e) {
    notify(`保存に失敗しました: ${String(e)}`, "error");
  }
}

/**
 * アクティブタブを指定エンコーディングで開き直す（要件5.6 Reopen）。
 *
 * 自動判定の誤り/曖昧（Shift_JIS 誤判定・BOM なし日本語 UTF-16 等）をユーザー選択で上書きする。
 * dirty タブはバッファを差し替えるため破棄確認を出す（最上位原則「データを失わない」）。成功後は
 * タブの encoding/has_bom/line_ending を新値へ更新し、以後の save_document がそのエンコーディングを
 * 維持する（要件5.2）。失敗時（妥当でないエンコーディング等）は現状を一切変えずトースト通知する。
 */
async function reopenActiveWithEncoding(enc: DocEncoding): Promise<void> {
  const path = state.active;
  if (!path || !state.editor) return;
  const tab = state.tabs.find((t) => t.path === path);
  if (!tab) return;
  // dirty タブ保護: 未保存編集をバッファ差し替えで失う前に破棄確認する（最優先原則）。
  // window.confirm はこの Tauri/WebView2 ビルドでダイアログを出さず即 true を返す（confirmModal 新設の
  // 根拠・src/main.ts 671 行）。素通しで無断破棄しないよう自前モーダルで明示確認する。
  if (tab.dirty) {
    const ok = await confirmModal(
      `「${tab.title}」には未保存の変更があります。\n` +
        `${encodingLabel(enc, false)} で開き直すと、未保存の変更は破棄されます。続けますか？`,
      { okLabel: "開き直す", danger: true },
    );
    if (!ok) return;
  }
  try {
    const doc = await reopenDocumentWithEncoding(path, enc);
    // await 中に別タブへ切り替わっていたら以後の差し替えを行わない（activateTab と同じ最新性ガード）。
    if (state.active !== path) return;
    // タブのエンコーディング/BOM/改行を新値へ（以後の保存がこれを維持する＝要件5.2）。
    tab.encoding = doc.encoding;
    tab.hasBom = doc.has_bom;
    tab.lineEnding = doc.line_ending;
    tab.editingOff = doc.degrade.editing_off;
    // 破棄確認済み: dirty/draft をクリアし開いた内容を正にする。
    tab.dirty = false;
    tab.draft = undefined;
    // エディタを開いた内容で作り直す（activateTab の載せ替えと同じ作法）。
    state.editor?.destroy();
    state.editor = createEditor({
      parent: editorHost(),
      initialDoc: doc.text,
      onChange: () => markDirty(path),
      onCursorChange: () => refreshStatus(),
      lineWrapping: state.lineWrapping,
      tabWidth: state.tabWidth,
      // 開き直し後も段階制（highlight_off/editing_off）に従い重い装飾の要否を決める。
      heavy: !(doc.degrade.highlight_off || doc.degrade.editing_off),
      // エディタ→プレビュー片方向スクロール同期（S4・要件6.1 改訂）。
      onScroll: onEditorScroll,
      // 言語選択（拡張子で markdown/HTML を切替・要件5.1）。
      filePath: path,
    });
    // 開いた内容のハッシュを基準に取り直し未読をクリアする（復元の別物判定の素・eval high）。
    void captureTabHash(path, doc.text);
    state.unread.clearFile(path);
    refreshTabs();
    refreshStatus();
    if (state.diffOn) await renderActiveDiff();
    if (resolveOccupancy(state.viewMode, state.diffOn).showPreview) {
      await renderActivePreview();
    }
    notify(`${encodingLabel(enc, false)} で開き直しました`, "info");
  } catch (e) {
    notify(`${encodingLabel(enc, false)} で開き直せませんでした: ${String(e)}`, "error");
  }
}

/** 差分トグル（Ctrl+Shift+D・要件8.2）。ON でアクティブタブの差分を読み取り専用表示する（独立トグル）。 */
async function onToggleDiff(): Promise<void> {
  if (!state.active) return;
  state.diffOn = !state.diffOn;
  // tab-tools の差分トグル表示（.on＋aria-pressed）を実状態へ同期する。
  refreshViewTools();
  if (state.diffOn) {
    await renderActiveDiff();
  } else {
    hideDiff();
  }
}

/** アクティブタブの差分（ベースライン vs 編集バッファ/ディスク）を計算し描画する（要件8.2）。 */
async function renderActiveDiff(): Promise<void> {
  if (!state.active) return;
  // 対象パスを冒頭で固定する（eval medium #28）。await（readFile/computeFileDiff）中にタブ切替・差分
  // トグル連打が起きると state.active は別パスへ動きうる。プレビュー（previewSerializer）と同じ流儀で
  // 最新性ガードを入れ、古い差分結果が後着して別タブの差分を描画するのを防ぐ。
  const path = state.active;
  // タブで開いていれば編集バッファ、なければディスク内容を current に渡す（要件8.2）。
  const current = state.editor ? state.editor.getContent() : await readFile(path);
  // readFile 後に別タブへ動いていたら破棄（後着の差分で別タブを汚さない）。
  if (state.active !== path) return;
  try {
    const diff = await computeFileDiff(path, current);
    // computeFileDiff 後にも最新性を再確認（差分計算中の切替も破棄する）。
    if (state.active !== path) return;
    state.diff?.destroy();
    state.diff = renderDiff(diffHost(), diff);
    // 占有はモード×差分トグルの直交で解決する（プレビュー+差分は左右並置＝ui-design 8章）。
    applyOccupancy();
    // 差分占有時は構造化ステータスを全体（差分あり数・行・文字）のみで描画する（カーソルは出さない）。
    refreshStatus();
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
  // tab-tools の差分トグル表示（.on/aria-pressed）を OFF へ同期する。
  refreshViewTools();
  // 差分 OFF 後はモードに応じてエディタ/プレビューへ占有を戻す（直交）。
  applyOccupancy();
}

/**
 * 表示モードを設定する（ソース/分割/プレビュー＝要件6.1・ui-design 8章）。tab-tools セグメント・
 * ショートカット双方の唯一の入口。モードは排他、差分トグル（state.diffOn）とは直交。
 *
 * プレビュー/分割は権限ゼロ別WebView へ custom protocol(pika-preview://)直配信した URL を backend から
 * 得て占有を更新する（HTML 本体はメインWebView を一切経由しない＝design doc 6章）。系統A/B 切替は
 * 世代カウンタで直列化し、古い prepare_preview 結果が後から来ても破棄して前モード残留を防ぐ。
 *
 * 占有を更新（applyOccupancy）した上で、プレビューを表示すべき占有（preview/split で showPreview）なら
 * 別WebView を新内容へナビゲートする（renderActivePreview）。セグメント表示は refreshViewTools が同期。
 */
async function setViewMode(mode: ViewMode): Promise<void> {
  if (!state.active) return;
  state.viewMode = mode;
  // tab-tools セグメントの .on/aria-pressed を実モードへ同期する。
  refreshViewTools();
  applyOccupancy();
  // プレビューを占有するモード（preview / split 差分OFF）になったら別WebView をナビゲートする。
  if (resolveOccupancy(state.viewMode, state.diffOn).showPreview) {
    await renderActivePreview();
  }
}

/**
 * プレビュー表示の切替（Ctrl+E・要件6.1）。ソース ⇔ プレビューをトグルする（ショートカット用）。
 * 分割中に Ctrl+E を押したらプレビュー単独へ寄せ、プレビュー中なら（編集できる）ソースへ戻す。
 */
async function onTogglePreview(): Promise<void> {
  if (!state.active) return;
  await setViewMode(state.viewMode === "preview" ? "source" : "preview");
}

/**
 * 分割表示の切替（Ctrl+\・代替 Ctrl+Shift+E・要件11.2）。ソース ⇔ 分割をトグルする（ショートカット用）。
 * 分割中なら（編集に専念できる）ソースへ戻し、それ以外（ソース/プレビュー）なら分割へ寄せる。
 */
async function onToggleSplit(): Promise<void> {
  if (!state.active) return;
  await setViewMode(state.viewMode === "split" ? "source" : "split");
}

/**
 * 3モード×差分トグルの直交占有を DOM へ適用する（要件6.1・ui-design 8章）。
 * S8 で意味単位（画像専有/検索バー追従/host 可視/split 属性/プレビュー追従）の小関数へ分割した。
 * 実行順序・結果 DOM・検索同期・host 可視・別WebView の出し分けは分割前と一切変えていない（挙動不変）。
 */
function applyOccupancy(): void {
  // 非テキスト（画像）が占有なら image-host を専有して早期復帰（モード/差分/プレビューは無い）。
  if (applyImageOccupancyIfActive()) return;
  // 非画像占有（テキスト/差分/プレビュー）では image-host を必ず隠す（画像→テキスト切替の正規化）。
  imageHost().hidden = true;
  const occ = resolveOccupancy(state.viewMode, state.diffOn);
  syncSearchBarForOccupancy();
  applyHostVisibility(occ);
  applySplitAttribute(occ);
  // 占有（ソース/差分/プレビュー）が変わったらステータスのカーソル有無も切り替える（要件11.1）。
  // ソース占有のみカーソル位置を出し、差分/プレビュー占有では全体（差分・行・文字）のみにする。
  refreshStatus();
  syncPreviewForOccupancy(occ);
}

/**
 * 非テキスト（画像/非対応バイナリ＝要件12.2・U3）が占有なら image-host を専有して true を返す。
 * 画像にモード/差分/プレビューは無いので resolveOccupancy より前に分岐する（呼び出し側は true で早期復帰）。
 */
function applyImageOccupancyIfActive(): boolean {
  const activeTab = state.tabs.find((t) => t.path === state.active);
  if (!(activeTab && activeTab.nonText && !activeTab.deleted)) return false;
  editorHost().hidden = true;
  diffHost().hidden = true;
  previewHost().hidden = true;
  imageHost().hidden = false;
  editorPane().removeAttribute("data-split");
  // 画像占有ではエディタが消えるので検索バーも閉じる（canSearch=false と整合・古いハイライト残さない）。
  if (searchController?.isOpen()) searchController.close();
  // プレビュー子WebView は OS 子ウィンドウで z-index 制御不可＝画像占有中は必ず隠す（手前に残らない）。
  void hidePreview();
  // editor 無しなので no-op（カーソル/行/文字を出さない）。状態提示は image-host の描画自体が担う。
  refreshStatus();
  return true;
}

/**
 * 検索バーが開いている間に占有が変わったときの追従（S5）:
 * - エディタも差分も検索対象でない占有（プレビューのみ/画像）へ移ったら閉じる（プレビュー内検索は系統C）。
 * - ソース⇄差分の切替や差分の再描画（外部変更で差分ハンドルが作り直される）では、ハイライト先を
 *   現在の対象（CM6 or 差分DOM）へ貼り直す（refresh＝現クエリで再検索し新ハンドルへ再適用する）。
 */
function syncSearchBarForOccupancy(): void {
  if (searchController?.isOpen()) {
    if (!canSearchView()) searchController.close();
    else searchController.refresh();
  }
}

/** 占有に応じて 3 つの host（エディタ/差分/プレビュー）の可視を切り替える。 */
function applyHostVisibility(occ: ReturnType<typeof resolveOccupancy>): void {
  editorHost().hidden = !occ.showEditor;
  diffHost().hidden = !occ.showDiff;
  previewHost().hidden = !occ.showPreview;
}

/**
 * 左右並置（split）の出し分け（ui-design 8章）。occ は直交占有の結果なので、
 *  - プレビュー＋差分（preview/split で差分ON）→ 左＝レンダリング／右＝テキスト差分。
 *  - 分割＋差分OFF（split で showEditor && showPreview）→ 左＝エディタ／右＝プレビュー。
 *  - それ以外（単独占有）→ data-split を外し既存の単独レイアウト（grid-row:2 を 1 要素が占有）に戻す。
 */
function applySplitAttribute(occ: ReturnType<typeof resolveOccupancy>): void {
  if (occ.showPreview && occ.showDiff) {
    editorPane().setAttribute("data-split", "preview-diff");
  } else if (occ.showPreview && occ.showEditor) {
    editorPane().setAttribute("data-split", "editor-preview");
  } else {
    editorPane().removeAttribute("data-split");
  }
}

/**
 * 占有がプレビュー非表示なら別WebView を隠す（表示は renderActivePreview の show_preview が担う）。
 * 占有がプレビュー表示でも、ここでは bounds 追従のみ更新する（ナビゲートは renderActivePreview）。
 */
function syncPreviewForOccupancy(occ: ReturnType<typeof resolveOccupancy>): void {
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
 * エディタ→プレビュー片方向スクロール同期を発火してよい状態か（S4・要件6.1 改訂）。
 *
 * 発火条件（プランの Acceptance）:
 * - アクティブタブが **Markdown**（`.md`/`.markdown`）であること。HTML（`.html`/`.htm`）・SVG（`.svg`）は
 *   `data-sourcepos` を持たず行マッピングできないため対象外（要件6.1・HTML/SVG は独立スクロール）。
 * - プレビューが可視（split / preview の showPreview）であること。
 * - **差分OFF**であること（差分ON時は左=レンダリング/右=テキスト差分で同期しない）。
 */
function isMarkdownPreviewActive(): boolean {
  if (!state.active) return false;
  if (state.diffOn) return false;
  if (!resolveOccupancy(state.viewMode, state.diffOn).showPreview) return false;
  const lower = state.active.toLowerCase();
  return lower.endsWith(".md") || lower.endsWith(".markdown");
}

/** rAF スロットルの保留フラグ（毎スクロールで IPC を撃たない＝固まらない・S4 risk 緩和）。 */
let scrollSyncPending = false;

/**
 * エディタのスクロールをプレビューへ片方向同期する（S4・要件6.1 改訂）。
 *
 * `.cm-scroller` の native scroll（createEditor の onScroll）から呼ばれる。毎フレームに 1 回へ rAF で
 * スロットルし、Markdown プレビュー可視かつ差分OFF のときだけ最上部の表示行（getScrollTop）を
 * `sync_preview_scroll` へ渡す（backend が別WebView の data-sourcepos ブロックへスクロール）。
 * 逆方向（プレビュー→エディタ）は F-029 で持たない（片方向のみ）。
 */
function onEditorScroll(): void {
  // 発火しない状態（非 Markdown/プレビュー非表示/差分ON）なら rAF も予約しない（無駄を作らない）。
  if (!isMarkdownPreviewActive()) return;
  if (scrollSyncPending) return;
  scrollSyncPending = true;
  requestAnimationFrame(() => {
    scrollSyncPending = false;
    // rAF の間にタブ/モード/差分が変わって対象外になっていたら撃たない（世代・表示モード変更ガード）。
    if (!state.editor || !isMarkdownPreviewActive()) return;
    const line = Math.max(1, Math.floor(state.editor.getScrollTop()));
    void syncPreviewScroll(line);
  });
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
      notify("JS を使うHTMLのため表示が崩れる可能性があります（既定のアプリで開けます）", "warn");
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
    // プレビュー占有時は構造化ステータスを全体（差分あり数・行・文字）のみで描画する（カーソルは出さない）。
    refreshStatus();
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
        // 未読解除・ツリー/タブのマーク解除（要件8.3）。新ベースライン基準で差分も貼り直す。
        state.unread.clearFile(path);
        await refreshAll();
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
      if (result.skipped === 0) {
        // 全件確定。フリーズ集合をそのまま未読解除する（最も単純で正しい）。
        for (const path of targets) state.unread.clearFile(path);
      } else if (result.updated > 0) {
        // 1件でもスキップがある混在ケース（eval medium #26）。
        // 旧実装は「skipped===0 のときだけ全クリア」だったため、混在時は確定済み（updated 件）の
        // 未読まで残し続け UI と backend が乖離していた。confirm_all の戻り値は件数のみで「どのパスを
        // 確定したか」を返さないため、ここでは backend 真実へ再同期する：確定済みパスは backend が
        // ベースラインを現ディスク内容へ更新済みなので、再度ディスク内容で差分を取れば change_count===0
        // になる。スキップ（処理中に変化）したパスは依然差分が残る（>0）。この per-path 再照合で
        // 確定できたパスだけを正確に未読解除する。退避は backend 側で取れているのでデータ喪失はない。
        // 各パスは独立なので並行化して IPC ラウンドトリップの直列線形劣化を避ける。
        const confirmedFlags = await Promise.all(
          targets.map(async (path) => {
            try {
              const disk = await readFile(path);
              const fresh = await computeFileDiff(path, disk);
              return fresh.change_count === 0;
            } catch {
              // 再照合に失敗したパスは安全側で未読のまま残す（誤って既読化しない）。
              return false;
            }
          }),
        );
        targets.forEach((path, i) => {
          if (confirmedFlags[i]) state.unread.clearFile(path);
        });
      }
      // updated===0（全スキップ）のときは未読を一切触らない（全件未確定のまま残す）。
      await refreshAll();
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
      // 巻き戻しは内容も変わるので行/文字も再計測（refreshAll が status を更新）。
      await refreshAll();
      notify("確認済み時点に戻しました（戻す前の内容は退避済み）");
    } catch (e) {
      notify(`巻き戻しできません: ${String(e)}`, "error");
    }
  });
}

/** 外部変更（fs-changed）を未読ストアへ適用し、ツリー/タブを再描画する（要件4.2/7.2）。 */
function onExternalChange(changes: import("./ipc").FsChange[]): void {
  if (changes.length === 0) return;
  // ツリーから自分で作成したばかりのファイルの created は未読(◆)にしない（指摘4）。watcher は workspace を
  // 再帰監視するため自作成にも created が飛ぶ。到着時点で selfCreatedPaths を消費して未読化を抑止する
  //（非同期到着と競合しないワンショット）。modified/removed 等はそのまま反映する。
  // 先に TTL を過ぎた未消費エントリを掃除し、stale エントリが後続の正当な外部 created を誤抑制しないようにする。
  pruneSelfCreated(Date.now());
  const effective = changes.filter(
    (c) => !(c.kind === "created" && selfCreatedPaths.delete(selfKey(c.path))),
  );
  if (effective.length === 0) return;
  state.unread.apply(effective);
  // 開いているクリーン（未保存変更なし）タブへの外部変更は自動リロード（要件7.2）。
  // 未保存変更があるタブは自動リロードせず通知のみ（衝突処理は sprint 3 で本実装）。
  void autoReloadCleanTabs(effective);
  refreshTree();
  refreshTabs();
  // 差分あり数が変わったので構造化ステータス（差分 N …）を更新する。表示専用ステータスとは別に
  // 未読件数を読み上げ領域へ announce する（要件11.5）。
  if (state.folder) {
    const count = state.unread.unreadCount();
    refreshStatus();
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
    // 検索バーを開いたまま外部リロードされた場合、件数/ハイライトを新内容へ追従させる
    // （reloadExternal は decoration を map するだけで再検索しないため stale 化する）。
    // 閉じている/空クエリなら refresh は no-op。
    searchController?.refresh();
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

// ── 表示メニューの追加操作（折り返し/テーマ/ログフォルダ・UIブラッシュアップ T8）─────────────

/**
 * settings.toml の有効設定のうち**ユーザーが見て効く4項目**を即適用する（要件10.3/10.4 再起動なし反映・U2a）。
 *
 * 適用タイミングの違い（取り違え厳禁）:
 * - 共通（起動時＋ライブ両方）: theme / wrap_default / tab_width。settings-changed のたびに反映する。
 * - 起動時のみ（opts.initial===true）: default_mode。「初回既定」の意味であり、ライブで適用すると
 *   現在開いているビューを強制切替してしまうため、settings-changed では viewMode を触らない。
 *
 * wrap_default/tab_width はアプリ全体の state（lineWrapping/tabWidth）へ代入し、既存エディタがあれば
 * Compartment 差し替えで即反映する（内容/カーソル/スクロール/履歴は壊さない）。エディタを作り直しても
 * createEditor の初期値で引き継ぐ。ユーザーが表示メニューで手動トグルした後に settings を編集した場合は
 * settings 値で上書きされるが、設定編集はユーザーの明示的な意思表示なので許容する。
 *
 * 残りの backend 消費分（excluded_dirs/huge_file_threshold/sensitive_patterns/allow_remote_resources/
 * feature_mermaid 等の feature_ 群/full_hash_on_startup）は本タスク（U2a）のスコープ外で、U2b で別途適用する。
 */
function applySettings(s: Settings, opts: { initial: boolean }): void {
  // theme: ThemeSetting と ThemeMode は同型（"light"|"dark"|"system"）なので変換不要でそのまま渡せる。
  applyTheme(s.theme);
  // wrap_default: アプリ全体の折り返し設定へ反映し、開いているエディタがあれば即時切替する。
  state.lineWrapping = s.wrap_default;
  state.editor?.setLineWrapping(s.wrap_default);
  // tab_width: タブの**表示幅**（EditorState.tabSize）にのみ使う。挿入文字はタブ文字のまま（要件5.2）。
  state.tabWidth = s.tab_width;
  state.editor?.setTabWidth(s.tab_width);
  // default_mode: 起動時のみ初回既定として viewMode を確定する（既定 source を維持・要件6.1）。
  // restoreOnStartup がタブを開く前に viewMode が確定している必要があるため、初期化時に適用する。
  if (opts.initial) {
    state.viewMode = s.default_mode === "preview" ? "preview" : "source";
  }
}

/**
 * 行の折り返しトグル（表示メニュー・UIブラッシュアップ T8）。アプリ全体で 1 つの設定を反転し、
 * 現在エディタへ即時反映する（Compartment 差し替え＝内容/カーソル/スクロール/履歴を壊さない）。
 * タブ切替でエディタを作り直しても createEditor の初期値で引き継ぐ。
 */
function onToggleWrap(): void {
  state.lineWrapping = !state.lineWrapping;
  state.editor?.setLineWrapping(state.lineWrapping);
  notify(state.lineWrapping ? "折り返し: ON" : "折り返し: OFF", "info");
}

/** テーマを設定する（表示メニュー・ライト/ダーク/システム）。html[data-theme] を切替える。 */
function onSetTheme(mode: ThemeMode): void {
  applyTheme(mode);
}

/**
 * ログフォルダを開く（ファイルメニュー・要件12.3/design G・UIブラッシュアップ T9）。
 * backend の自前 command（open_log_folder）が OS（エクスプローラー）で <データルート>/logs/ を開く。
 * 対象は pika のログフォルダに固定（frontend からパスを渡さない＝任意フォルダ閲覧の導線にしない）。
 * 失敗時はパス取得にフォールバックして案内し、行き止まりにしない。
 */
async function onOpenLogFolder(): Promise<void> {
  try {
    await openLogFolder();
  } catch (e) {
    // 開けなかったときはせめてパスを案内する（行き止まりにしない・要件11.1）。
    try {
      const path = await logFolderPath();
      notify(`ログフォルダを開けませんでした: ${String(e)}（場所: ${path}）`, "error");
    } catch {
      notify(`ログフォルダを開けませんでした: ${String(e)}`, "error");
    }
  }
}

/**
 * 「既定のアプリで開く」（tab-tools・要件6.2/design G・UIブラッシュアップ T9）。
 * **現在アクティブなタブのファイル**を OS 既定アプリ（HTML なら既定ブラウザ・画像なら画像ビューア等）で開く。
 * 実態は「ブラウザ」ではなく拡張子の関連付けに従う「既定アプリ」起動なので UI/文言をそれに合わせる（指摘4）。
 * backend の自前 command（open_in_default_app）が絶対パス＋実在を再検証して起動する（fail-closed）。
 *
 * 新規（未保存で未作成）タブはディスクに実体が無く backend が拒否するため、保存を促して中断する
 * （行き止まりにしない）。任意 URL は開かない（対象は開いているファイルのみ）。
 */
async function onOpenExternal(): Promise<void> {
  const path = state.active;
  if (!path) return;
  const tab = state.tabs.find((t) => t.path === path);
  // 削除済み/未保存新規は実体が無い → 開く前に案内（backend も実在チェックで弾くが先に親切な導線を出す）。
  if (tab?.deleted) {
    notify("削除済みのファイルは開けません（［確認済み時点に戻す］で復元できます）", "warn");
    return;
  }
  try {
    await openInDefaultApp(path);
  } catch (e) {
    notify(`既定アプリで開けませんでした: ${String(e)}`, "error");
  }
}

/** バージョン表示（ヘルプメニュー）。 */
function onShowVersion(): void {
  notify("pika 0.1", "info");
}

// エンコーディング/改行コード表記（encodingLabel/lineEndingLabel）とメニュー定義（buildMenuSpecs）は
// src/ui/menu-specs.ts へ抽出した（S8・挙動不変）。encodingLabel は reopenActiveWithEncoding で使うため
// main も import する。buildMenuSpecs は main() で ctx（state＋各ハンドラ）を注入して呼ぶ。

/**
 * 単一インスタンス転送（要件3.4）。既に起動済みの pika に別プロセスが `pika <path>` を投げると
 * backend が named pipe で受け取り core 再検証した絶対パスを `open-request` で送ってくる。
 * 受理操作=パスオープン限定なので、ここでは「ファイルを開く（あれば -g 位置へ）」だけを行う。
 */
async function onOpenRequestEvent(payload: OpenRequestPayload): Promise<void> {
  // -g カーソル位置は paths 先頭ファイルに対応する規約（ipc.rs OpenRequest）。
  // 先頭パスを覚えておき、ループ内で「これが先頭か」を判定して goto を適用する（eval medium #27）。
  // 旧実装はループ後に先頭を **もう一度** openPath して二重に開き、かつ goto 対象がレースで
  // 別タブに当たりうる欠陥があった。ここでは二重オープンをやめ、各パスは一度だけ開く。
  const gotoTarget = payload.goto && payload.paths.length > 0 ? payload.paths[0] : null;
  for (const path of payload.paths) {
    await openPath(path);
    // 先頭パスを開いた直後（このパスがアクティブ＝state.active）にだけ goto を適用する。
    // openPath はファイルを開くとそのタブをアクティブ化する（openFile→activateTab・#10 の最新性
    // ガード適用済み）。activateTab が別タブへ切り替わっていたら適用しない（取り違え防止）。
    if (gotoTarget !== null && path === gotoTarget && payload.goto) {
      if (state.active === path && state.editor) {
        state.editor.gotoPosition(payload.goto.line, payload.goto.column ?? 1);
      }
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
  const name = basename(path);
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
  // ワークスペース外のファイルも開けるよう、読み取り許可域へ登録してから開く（要件3.2/9.1）。
  // ツリー経由の openFile は root 配下なので verify_read で既に通るが、ダイアログ/転送経由の
  // openPath は root 外を指しうるため allow_open_path で個別許可する（書込側 allowSavePath の対称）。
  // Tauri 不在（dev ブラウザ単体）等の失敗は握りつぶし、従来の read 経路に委ねる。
  try {
    await allowOpenPath(path);
  } catch {
    /* dev ブラウザ単体等で allow_open_path 不在 → read 側の再検証に委ねる。 */
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
  // この経路は activateTab を通らず直接 editor を破棄/再生成する。切替前に現アクティブタブの未保存編集を
  // draft へ退避しておく（eval high #11・データを失わない）。退避漏れがあると、dirty タブがアクティブな
  // ときに「存在しないパス」を開くと editor 破棄で未保存編集が消え、戻っても draft===undefined で
  // 黙ってディスク内容を読み直してしまう。activateTab/collectAppState と同じ前置きで全経路を airtight にする。
  captureActivePosition();
  state.active = path;
  state.editor?.destroy();
  state.editor = createEditor({
    parent: editorHost(),
    initialDoc: "",
    onChange: () => markDirty(path),
    onCursorChange: () => refreshStatus(),
    lineWrapping: state.lineWrapping,
    tabWidth: state.tabWidth,
    // 新規（空）ファイルは段階制対象でない＝重い装飾を有効にする。
    heavy: true,
    // エディタ→プレビュー片方向スクロール同期（S4・要件6.1 改訂）。新規 .md でも split で同期する。
    onScroll: onEditorScroll,
    // 言語選択（拡張子で markdown/HTML を切替・要件5.1）。
    filePath: path,
  });
  void captureTabHash(path, "");
  refreshTabs();
  // 新規ファイルは「保存で作成される」旨を一旦提示し、以後の編集（onCursorChange）で構造化ステータスへ移る。
  setStatus(`新規ファイル（保存で作成）: ${path}`);
  persistAppState();
}

// 状態収集/保存/復元（collectAppState/persistAppState/persistAppStateNow/restoreOnStartup/
// restoreTabPosition＋デバウンスタイマー）は src/app/persistence.ts へ抽出した（S8・挙動不変＝
// データを失わない不変条件を保持）。下の DI 結線で createPersistence に注入し、main は返り値の
// persistAppState/persistAppStateNow/restoreOnStartup/cancelPendingPersist を呼ぶ。

/**
 * いまフォーカスのあるペインを判定する（Ctrl+Enter 誤爆防止に使う＝要件11.2）。
 * 差分/プレビュー領域にフォーカスがあるときだけ Ctrl+Enter で「確認済み」を発火させる。
 */
/**
 * **エディタ（テキスト）編集**ができるビューか（U4/U5・要件5.4/5.5/5.1）。
 *
 * 条件: アクティブな**テキスト**タブが CM6 エディタとして見えていること。
 * - 画像/非対応バイナリ（nonText）・削除済み（deleted）・第2段階以降（editingOff）は CM6 が無い/編集不可。
 * - 差分ON（showEditor が落ちる）は false＝差分は読み取り専用。エディタ検索・置換・名前を付けて保存・
 *   Ctrl+G 行へ移動・太字/斜体/チェックボックスなど **エディタを書き換える/カーソル前提**の操作はこれで判定する。
 * → ソース／分割（エディタが可視）のときだけ true。
 */
function canSearch(): boolean {
  // canSearch はエディタが**可視**であることまで要求する（検索ハイライト/カーソル前提の編集系）。
  return canEditActiveText() && resolveOccupancy(state.viewMode, state.diffOn).showEditor;
}

/**
 * アクティブが編集可能なテキストタブか（CM6 エディタが生きている）。**ペイン可視は問わない**。
 *
 * canSearch との違いは末尾の `occupancy.showEditor` を見ない点だけ。「名前を付けて保存」のように
 * 現在のエディタ内容を別パスへ書き出すだけで**ペイン可視を必要としない**操作のゲートに使う
 * （プレーン保存と同様、プレビューのみモードでも効かせる＝Codex P2 の機能ギャップ是正）。
 */
function canEditActiveText(): boolean {
  if (!state.active || !state.editor) return false;
  const tab = state.tabs.find((t) => t.path === state.active);
  if (!tab || tab.nonText || tab.deleted || tab.editingOff) return false;
  return true;
}

/**
 * 差分ビュー内検索（S5・要件5.4 改訂）が有効か。
 *
 * 差分ON で差分面が可視（source/split/preview いずれの差分ONでも showDiff=true）かつ、表示中の差分に
 * 検索対象テキスト（連結表示テキスト）があるとき true。差分非対象（ハッシュのみ）・変更なしは
 * `DiffHandle.searchText()` が空を返すため false になる。差分は CM6 でないため独自 DOM へ別ハイライト
 * 機構（diff/index.ts）で強調する（置換は読み取り専用なので不可）。
 */
function diffSearchActive(): boolean {
  if (!state.diffOn || !state.diff) return false;
  if (!resolveOccupancy(state.viewMode, state.diffOn).showDiff) return false;
  const tab = state.tabs.find((t) => t.path === state.active);
  if (!tab || tab.deleted) return false;
  return state.diff.searchText().length > 0;
}

/**
 * 検索バーを開ける（＝Ctrl+F が機能する）ビューか（S5）。
 * エディタ検索（ソース/分割）に加え **差分ON** も許可する。置換・行へ移動・編集系は canSearch のまま。
 * プレビューのみ（差分OFF）・画像・第2段階は引き続き不可（プレビュー内検索は系統C 繰り越し）。
 */
function canSearchView(): boolean {
  return canSearch() || diffSearchActive();
}

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
      // ソース ⇔ プレビュー切替（差分トグルは直交なのでそのまま維持）。setViewMode が占有・別WebView・
      // tab-tools 表示（.on/aria-pressed）を一括同期する。
      void onTogglePreview();
      return true;
    case "toggle-split":
      // ソース ⇔ 分割切替（Ctrl+\・代替 Ctrl+Shift+E）。差分トグルとは直交（分割＋差分ON は
      // applyOccupancy が左プレビュー＋右差分へ倒す）。
      void onToggleSplit();
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
    case "goto-line":
      // 行へ移動（Ctrl+G・要件5.5）。canSearch ガード後に行番号入力モーダル→ジャンプ（onGotoLine 内）。
      void onGotoLine();
      return true;
    case "next-tab":
      // 次のタブへ（Ctrl+Tab・代替 Ctrl+PageDown・要件11.2）。1 枚以下なら何もしない（true で既定処理を抑止）。
      activateAdjacentTab(1);
      return true;
    case "prev-tab":
      // 前のタブへ（Ctrl+Shift+Tab・代替 Ctrl+PageUp・要件11.2）。
      activateAdjacentTab(-1);
      return true;
    case "find": {
      // 検索バーを開く（U4・要件5.4）。ソース/分割（エディタ）に加え **差分ON** でも開ける（S5）。
      // プレビューのみ/画像/第2段階は不可（プレビュー内検索は系統C 繰り越し）。編集メニューの検索も同経路。
      if (!canSearchView()) {
        notify("このビューでは検索できません（ソース/分割/差分で使えます）", "info");
        return true;
      }
      searchController?.open("find");
      return true;
    }
    case "replace": {
      // 置換バーを開く（U5・要件5.4）。置換はエディタを書き換えるためソース/分割限定（差分は読み取り専用）。
      if (!canSearch()) {
        notify(
          diffSearchActive()
            ? "差分は読み取り専用です（置換はソース/分割で使えます）"
            : "このビューでは置換できません（ソース/分割で使えます）",
          "info",
        );
        return true;
      }
      searchController?.open("replace");
      return true;
    }
  }
}

/**
 * ファイルを開く（Ctrl+O・要件11.2）。OS ネイティブ選択ダイアログでファイルを選ぶ
 * （dialog:allow-open・dev ブラウザ単体では window.prompt にフォールバック）。
 * キャンセル（null）時は何もしない。返却パスは openPath 経由（read_file の AccessControl 再検証）で開く。
 * 存在しないパスは openPath が新規タブとして開く（要件3.2）。
 */
async function onOpenFile(): Promise<void> {
  const picked = await pickNativePath({
    directory: false,
    defaultPath: state.folder ?? undefined,
    promptMessage: "開くファイルのパスを入力してください",
  });
  if (picked === null) return; // キャンセル＝何もしない。
  const trimmed = picked.trim();
  if (!trimmed) {
    notify("開くファイルのパスを入力してください", "warn");
    return;
  }
  await openPath(trimmed);
}

/** アクティブタブを閉じる（Ctrl+W・要件11.2）。任意パス版 closeTab へ委譲する。 */
function onCloseActiveTab(): void {
  if (!state.active) return;
  void closeTab(state.active);
}

/**
 * 行へ移動（Ctrl+G・要件5.5）。対話的に行番号を入力して指定行へジャンプする。
 *
 * 検索/置換と同じ条件（canSearch＝ソース/分割でエディタが可視かつ編集可能）でのみ使える。画像/差分/
 * プレビュー/第2段階では CM6 が無い/カーソルを持たないため案内のみ出す。入力モーダル（promptText）は
 * IME 合成ガード済み。空/非数値/1未満はやんわり通知して何もしない。
 */
async function onGotoLine(): Promise<void> {
  if (!canSearch()) {
    notify("このビューでは行へ移動できません（ソース/分割で使えます）", "info");
    return;
  }
  const input = await promptText("移動先の行番号", "例 42");
  if (input === null) return; // キャンセル。
  const n = parseInt(input.trim(), 10);
  if (!Number.isFinite(n) || n < 1) {
    notify("行番号を正しく入力してください（1 以上の整数）", "warn");
    return;
  }
  // gotoPosition は行数超過を最終行へクランプする（要件3.1 と同じ挙動）。桁は行頭（1）。
  state.editor?.gotoPosition(n, 1);
}

/**
 * 隣のタブへアクティブを移す（Ctrl+Tab=+1 / Ctrl+Shift+Tab=-1・要件11.2）。
 * 端は循環（先頭の前→末尾、末尾の次→先頭）。タブが 1 枚以下なら何もしない。
 */
function activateAdjacentTab(delta: number): void {
  const len = state.tabs.length;
  if (len <= 1) return;
  const idx = state.tabs.findIndex((t) => t.path === state.active);
  if (idx < 0) return;
  const nextIdx = (idx + delta + len) % len;
  void activateTab(state.tabs[nextIdx].path);
}

/**
 * 名前を付けて保存（save-as・要件5.5）。OS 保存ダイアログ（dialog:allow-save）で保存先を選び、
 * その保存先を許可域へ登録（allowSavePath）してから現在のエディタ内容を新パスへ書き出す。
 *
 * 順序（最上位原則「データを失わない」）: **保存が成功してからタブの path を差し替える**。途中で
 * キャンセル/失敗したら現タブはそのまま（旧パスを指したまま・未保存編集も保持）。表現不能文字での
 * 中断は onSave と同じく［UTF-8で保存／キャンセル］を提示する。
 */
async function onSaveAs(): Promise<void> {
  if (!state.active || !state.editor) return;
  const oldPath = state.active;
  const tab = state.tabs.find((t) => t.path === oldPath);
  if (!tab) return;
  const content = state.editor.getContent();
  // 保存先を選ぶ（dev ブラウザ単体では save() 不在のため prompt にフォールバック）。
  let picked: string | null;
  try {
    picked = await saveNativeDialog({ defaultPath: oldPath });
  } catch {
    picked = window.prompt("名前を付けて保存（保存先のパス）", oldPath);
  }
  if (picked === null) return; // キャンセル＝何もしない。
  const newPath = picked.trim();
  if (!newPath) {
    notify("保存先のパスを入力してください", "warn");
    return;
  }
  // 衝突検出（データを失わない・closeTab の「破棄は必ず確認」と挙動を揃える）: 保存先 newPath を
  // 既に**別タブ**で開いていて、それが未保存（dirty もしくは退避 draft あり）なら、save-as 成功後に
  // applySaveAsResult が無確認でそのタブを畳む＝未保存編集を silently 失う。これをディスク書込（withBusy）
  // の前に破棄確認する。No（キャンセル）なら save-as 自体を中止する（保存もしない）。
  if (newPath !== oldPath) {
    const dup = state.tabs.find((t) => t !== tab && t.path === newPath);
    if (dup && (dup.dirty || dup.draft !== undefined)) {
      const ok = await confirmModal(
        `別タブで開いている「${dup.title}」に未保存の変更があります。\n` +
          `保存先として上書きすると失われます。続けますか？`,
        { okLabel: "上書きする", danger: true },
      );
      if (!ok) return; // 破棄したくない＝save-as を中止（何もしない＝保存もしない）。
    }
  }
  await withBusy(async () => {
    // 現エンコーディングを維持して新パスへ書く。表現不能文字は UTF-8 で再試行可（onSave と同じ作法）。
    let encoding = tab.encoding;
    let hasBom = tab.hasBom;
    let forceUtf8 = false;
    for (;;) {
      // 保存先を書込許可へ登録する（ワークスペース外でもユーザー意図ゲートを経たパスのみ許可・要件5.5）。
      // backend 側は **one-shot**（次の 1 回の書込だけ・file-scoped）なので、表現不能文字の UTF-8 再試行
      // （ループ 2 周目）では前回の verify_write で消費済み＝毎回ここで張り直す必要がある。
      // 登録失敗（解決不能等）は握りつぶし、最終的な可否は saveDocument の verify_write に委ねる。
      try {
        await allowSavePath(newPath);
      } catch {
        // best-effort（後段 saveDocument が封じ込めで弾く）。
      }
      let result;
      try {
        result = await saveDocument(
          newPath,
          content,
          forceUtf8 ? "utf-8" : encoding,
          forceUtf8 ? false : hasBom,
          forceUtf8,
        );
      } catch (e) {
        notify(`保存に失敗しました: ${String(e)}`, "error");
        return;
      }
      if (result.status === "unmappable") {
        // 表現不能文字の確認は saveOnce と共通の confirmUtf8Fallback に集約（自前モーダルで明示確認）。
        const ok = await confirmUtf8Fallback(newPath, result.unmappable.length);
        if (!ok) {
          notify("保存を中止しました（表現不能文字のため・変更は保持しています）", "warn");
          return;
        }
        forceUtf8 = true;
        continue; // UTF-8 で再試行。
      }
      // 保存成功。UTF-8 強制を選んだら以後の元エンコーディングを UTF-8（BOM なし）へ確定する。
      if (forceUtf8) {
        encoding = "utf-8";
        hasBom = false;
      }
      break;
    }
    // === 保存成功後: タブ path 差替＋未読の付け替え（正しい順序）===
    applySaveAsResult(oldPath, newPath, content, encoding, hasBom);
    // 新パスの全既読ベースラインを backend に確立する（save-as 先は open していないので基準が無い）。
    // これを差分の貼り直しより先に await して「保存内容＝基準」を成立させ、差分トグル中でも壊れない。
    // 失敗（権限/巨大等）は握りつぶす＝保存自体は成功しており UI を妨げない。
    try {
      await openDocument(newPath);
    } catch {
      // ベースライン確立に失敗しても保存は成立済み。差分は「基準なし」表示になるだけ。
    }
    // 差分/プレビュー表示中に save-as したら、新パス基準で貼り直す（旧パスの差分/プレビューを残さない）。
    if (state.active === newPath) {
      if (state.diffOn) await renderActiveDiff();
      if (resolveOccupancy(state.viewMode, state.diffOn).showPreview) {
        await renderActivePreview();
      }
    }
  });
}

/**
 * save-as 成功後の状態更新（タブ path 差替・未読/差分基準の付け替え・要件5.5）。
 *
 * - 旧パスを指していたタブを新パスへ付け替え（title/encoding/dirty/draft も更新）。
 * - 既に新パスのタブが別に開いていたら重複を畳む（同一 path のタブを二重に持たない）。
 * - 未読は旧/新パスとも消す（保存直後＝全既読）。新パスは backend に基準が無いので openDocument で
 *   全既読ベースラインを確立し、差分トグル時に「保存内容＝基準」で正しく差分なしになるようにする。
 */
function applySaveAsResult(
  oldPath: string,
  newPath: string,
  content: string,
  encoding: DocEncoding,
  hasBom: boolean,
): void {
  const tab = state.tabs.find((t) => t.path === oldPath);
  if (!tab) return;
  // 別タブが既に新パスを開いていたら畳む（同一 path の二重タブを作らない）。自タブ自身（oldPath===newPath
  // の上書き save-as）は除外する。
  if (newPath !== oldPath) {
    const dup = state.tabs.find((t) => t !== tab && t.path === newPath);
    if (dup) {
      state.tabs = state.tabs.filter((t) => t !== dup);
      state.unread.clearFile(newPath);
    }
  }
  tab.path = newPath;
  tab.title = basename(newPath);
  tab.dirty = false;
  tab.draft = undefined;
  tab.deleted = false; // 実体を書いたので削除済みフラグは解除。
  tab.encoding = encoding;
  tab.hasBom = hasBom;
  // アクティブを新パスへ追従させる（保存したタブを見続ける）。
  if (state.active === oldPath) {
    state.active = newPath;
    notices.setActiveTab(newPath);
  }
  // 未読を旧/新パスとも消す（保存直後＝全既読・別物判定の素は captureTabHash が詰める）。
  state.unread.clearFile(oldPath);
  state.unread.clearFile(newPath);
  void captureTabHash(newPath, content);
  refreshTabs();
  refreshTree();
  refreshStatus();
  notify("名前を付けて保存しました");
  void persistAppState();
}

/**
 * 任意のタブを閉じる（Ctrl+W／タブの × クリック・要件11.2）。未保存があれば破棄確認を挟む
 * （データを失わない＝確認を必ず通す）。閉じたのがアクティブタブなら隣へアクティブを移す。
 */
async function closeTab(path: string): Promise<void> {
  const tab = state.tabs.find((t) => t.path === path);
  if (!tab) return;
  // 破棄確認は検索バーを閉じる前に行う（キャンセル時に検索バーだけ閉じる副作用を避ける）。
  // window.confirm はこの Tauri/WebView2 ビルドでダイアログを出さず即 true を返す（confirmModal 新設の
  // 根拠・src/main.ts 671 行）。素通しで無断破棄しないよう自前モーダルで明示確認する。
  if (tab.dirty) {
    const name = tab.title;
    const ok = await confirmModal(
      `「${name}」に未保存の変更があります。保存せずに閉じると失われます。閉じますか？`,
      { okLabel: "閉じる", danger: true },
    );
    if (!ok) return;
  }
  // タブを閉じるとエディタが作り直される/畳まれるので検索バーを閉じる（古いハイライトを残さない）。
  searchController?.close();
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
  //
  // 意図（eval medium #29）: **右隣を優先**する。VSCode/一般エディタの慣習で、タブを閉じると
  // すぐ右のタブへ移る（末尾を閉じたときだけ左隣＝新しい末尾へ移る）。
  // フィルタ後の配列では、閉じたタブの右隣は元の id+1 が左へ詰まって **新配列の idx** に来る。
  // 末尾を閉じた（idx === newTabs.length）ときは新配列に idx が無いので、Math.min で末尾
  // （= 元の左隣）へクランプする。空配列（最後の1枚を閉じた）は -1 になり next=null へ落ちる。
  const newTabs = state.tabs;
  const nextIdx = newTabs.length === 0 ? -1 : Math.min(idx, newTabs.length - 1);
  const next = nextIdx >= 0 ? newTabs[nextIdx] : null;
  if (next) {
    void activateTab(next.path);
  } else {
    state.active = null;
    state.editor?.destroy();
    // 空のエディタを残す（タブが無くてもキーボード操作の到達先を保つ）。
    state.editor = createEditor({
      parent: editorHost(),
      initialDoc: "",
      onChange: () => undefined,
      lineWrapping: state.lineWrapping,
      tabWidth: state.tabWidth,
      // 空エディタ（タブ無し）は段階制対象でない＝重い装飾を有効にする。
      heavy: true,
      // エディタ→プレビュー片方向スクロール同期（S4）。タブ無しなので実質発火しない（ガードで no-op）。
      onScroll: onEditorScroll,
    });
    // 直前が画像タブだった場合に image-host が残らないよう host 可視を正規化する（画像→空状態の正規化・U3）。
    // state.active=null なので applyOccupancy は image-host を隠し空エディタ（editor-host）を出す。
    applyOccupancy();
    refreshTabs();
    setStatus(emptyMessage("no-folder"));
  }
  void persistAppState();
}

// ── DI 結線（S8・main.ts モノリス分割）─────────────────────────────────────────────────────
// 抽出した3モジュール（ui/tree-actions・app/persistence・ui/menu-specs）へ state とハンドラを注入する。
// state は単一の可変オブジェクトへの**参照**を渡し（挙動不変＝データ損失防止の不変条件を保持）、main 在籍の
// 各関数（hoisted）をコールバックとして渡す。createTreeActions/createPersistence の呼び出しは関数を
// 組み立てるだけで実行はしない（実際の呼び出しは runtime＝module 評価完了後）ので TDZ にならない。
// menu-specs は buildMenuSpecs の呼び出しが 1 度だけなので main() 内で ctx を組んで結線する。

// ツリー操作（新規作成/削除/コンテキストメニュー）。selfCreatedPaths は main が保持し、登録だけ注入する
// （消費は onExternalChange が担う）。挙動は tree-actions 抽出前と不変。
const { showTreeContextMenu, showRootContextMenu } = createTreeActions({
  state,
  refreshTree,
  openFile,
  markSelfCreated,
});

// 状態の収集/保存/復元（state.json）。退避先行/最新性ガード/デバウンス/復元順序は persistence 内で不変。
const { persistAppState, persistAppStateNow, restoreOnStartup, cancelPendingPersist } =
  createPersistence({
    state,
    captureActivePosition,
    refreshTree,
    refreshTabs,
    updateTreeHeader,
    newTab,
    openFile,
    activateTab,
  });

async function main(): Promise<void> {
  initTheme();
  // ARIA 全Web再構築の初期化（F6/Shift+F6 ペイン間フォーカス循環・ランドマーク確実化＝要件11.5・design doc 17章）。
  initA11y();
  // カスタムメニューバー（UIブラッシュアップ T8）。ツールバー(#toolbar)は廃止し、フォルダを開く/保存/
  // すべて確認済み/巻き戻し/表示モード/折り返し/テーマ/再同期/バージョンを HTML/CSS/TS のメニューへ集約する。
  // 各メニューの活性・✓・現在値（エンコーディング/改行/テーマ/折り返し）は build が開くたびに評価する。
  const menubarEl = document.getElementById("menubar") as HTMLElement;
  const menuLayerEl = document.getElementById("menu-layer") as HTMLElement;
  // メニュー定義（ui/menu-specs）へ state と各ハンドラを注入する（buildMenuSpecs は 1 度だけ呼ぶ）。
  // メニュー構成・有効無効条件・各アクションの呼び出し先は menu-specs 抽出前と一切変えていない（挙動不変）。
  initMenuBar(
    menubarEl,
    menuLayerEl,
    buildMenuSpecs({
      state,
      onOpenFolder,
      onOpenFile,
      onSave,
      onSaveAs,
      onOpenLogFolder,
      onConfirmAll,
      onRollback,
      onToggleDiff,
      onToggleWrap,
      onSetTheme,
      onF5,
      onShowVersion,
      setViewMode,
      reopenActiveWithEncoding,
      dispatchAction,
      canEditActiveText,
      canSearch,
    }),
  );
  // フレームレス化の自前ウィンドウ操作（最小化/最大化/閉じる・ui-design §7）。ドラッグ移動は
  // メニュー帯の data-tauri-drag-region（index.html）が担う。
  initWindowControls();
  // 検索/置換バー（U4/U5・要件5.4）。deps は getter 経由で常に最新の state.editor / 内容を取る
  //（タブ切替で editor を作り直しても正しいエディタへ向く）。バー実体は #editor-pane の右上へ重ねる。
  searchController = createSearchController({
    editorPane: editorPane(),
    getEditor: () => state.editor,
    // 検索対象テキスト: 差分ビュー内検索（S5）が有効なら差分の連結表示テキスト、ほかはエディタ内容。
    getContent: () =>
      diffSearchActive()
        ? (state.diff?.searchText() ?? null)
        : (state.editor?.getContent() ?? null),
    // バーを開ける条件は差分ON も含める（置換・行へ移動などエディタ前提の操作は canSearch のまま）。
    canSearch: canSearchView,
    notify,
    // ── 差分ビュー内検索（S5）。ハイライト/ジャンプ/クリアを現在の差分ハンドルへ委譲 ─────────────
    isDiffSearch: diffSearchActive,
    setDiffMatches: (matches, current) => state.diff?.setSearchMatches(matches, current),
    jumpDiffMatch: (index) => state.diff?.jumpToHit(index),
    // 差分検索の件数乖離防止（S5）: 差分DOM に出せないヒット（改行/ゼロ幅のみ）を件数に数える前に除外する。
    filterDiffRenderable: (m) => state.diff?.filterRenderable(m) ?? m,
    clearDiffSearch: () => state.diff?.clearSearch(),
    focusDiff: () => diffHost().focus(),
  });
  // tab-tools: モード切替セグメント（ソース/分割/プレビュー）。data-mode を読んで setViewMode へ流す。
  for (const btn of modeButtons()) {
    btn.addEventListener("click", () => {
      const mode = btn.dataset.mode as ViewMode | undefined;
      if (mode) void setViewMode(mode);
    });
  }
  // tab-tools: 差分トグル（独立）・確認済みにする（T6 で移設済みの常設ボタン）。
  toggleDiffBtn().addEventListener("click", () => void onToggleDiff());
  confirmBtn().addEventListener("click", () => void onConfirm());
  // tab-tools: ブラウザで開く（T9）。アクティブタブのファイルを OS 既定アプリで開く。
  openExternalBtn().addEventListener("click", () => void onOpenExternal());
  // tab-tools: 全タブ一覧ドロップダウン（要件5.3）。状態マーク付きで全タブを列挙し選択で切替える。
  tabListBtn().addEventListener("click", () => showAllTabsMenu(tabListBtn()));
  // 隠れ未読バッジ（T10・差分 C5）。タブ列の横スクロールで可視範囲が変わるたびに再計算する
  // （更新タイミング(a)）。scroll は高頻度なので rAF デバウンスへ流す。passive で滑らかに。
  tabsEl().addEventListener("scroll", () => scheduleHiddenUnread(), { passive: true });
  // タブのオーバーフローは横スクロールバーを消し（CSS）、縦ホイール（deltaY）を横スクロールへ変換する。
  // タッチパッドの横スワイプ（deltaX）は既定の横スクロールへ委ねるため deltaY のみ拾う。
  // 変換時のみ既定の縦スクロールを抑止する（passive:false）。scrollLeft 変化で scroll イベント→
  // scheduleHiddenUnread が走り、隠れ未読バッジは自然に再計算される。
  tabsEl().addEventListener(
    "wheel",
    (e) => {
      if (e.deltaY !== 0) {
        tabsEl().scrollLeft += e.deltaY;
        e.preventDefault();
      }
    },
    { passive: false },
  );
  // バッジクリック（親切機能）: 最初の隠れ未読タブが見える位置へ #tabs をスクロールする。
  hiddenUnreadBadge().addEventListener("click", () => scrollToHiddenUnread());
  // ツリー収納/引き出しトグル（B5・ui-design 7章）。ヘッダ右端「‹」で収納、レール「›」で引き出す。
  treeCollapseBtn().addEventListener("click", () => setTreeCollapsed(true));
  treeExpandBtn().addEventListener("click", () => setTreeCollapsed(false));
  // ツリーの空き領域（行以外）を右クリックしたら、ルートへの新規作成メニューを出す（T5）。
  // 行（treeitem）上の右クリックは tree.ts が stopPropagation で先に処理するためここへは来ない。
  document.getElementById("tree-pane")?.addEventListener("contextmenu", (e) => {
    e.preventDefault();
    showRootContextMenu(e.clientX, e.clientY);
  });
  // ツリーヘッダ（B4）の初期表示（未オープン時は素の文言）。復元後に更新される。
  updateTreeHeader();
  // tab-tools セグメントの初期表示（既定モード=ソースに .on を付け、タブ未オープン時は無効化する）。
  // 以後はタブ操作/モード切替/差分トグルのたびに refreshViewTools が同期する。
  refreshViewTools();
  // 主要ショートカットを単一のキーディスパッチ表で処理する（要件11.2・eval high: shortcuts 配線）。
  // 判定（どのキーが何の操作か・Ctrl+Enter 誤爆防止・代替割当）は shortcuts.resolveShortcut
  // （pika-core::shortcuts の写し）へ集約し、ここは結果（Action）を dispatchAction へ流すだけ。
  // F6/Shift+F6（ペイン間フォーカス循環）は a11y/index.ts が capture フェーズで先取りする。
  window.addEventListener("keydown", (e) => {
    // IME（日本語入力）変換中のキーはショートカット判定に回さない（誤爆ガード）。変換確定の Enter や
    // 変換候補操作中のキーで Action が暴発しないよう、合成中（isComposing）/合成キー（keyCode 229）は委譲する。
    if (e.isComposing || e.keyCode === 229) return;
    // モーダル（名前入力プロンプト/確認）表示中はグローバルショートカットを無効化する（指摘5）。
    // 名前入力中の Ctrl+Shift+Enter（確認済み）/ Ctrl+W（タブを閉じる）等が背景タブへ貫通しない。
    if (isModalOpen()) return;
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
  window.addEventListener("resize", () => {
    syncPreviewBounds();
    // ウィンドウ幅が変わると可視タブ範囲も変わるので隠れ未読バッジを再計算する（更新タイミング(c)）。
    scheduleHiddenUnread();
  });
  // 初期表示時にも一度バッジ状態を確定させる（復元でタブが復元されている場合に備える）。
  scheduleHiddenUnread();
  // 外部変更/監視モードの購読（backend の emit を受ける）。
  await onFsChanged((payload) => onExternalChange(payload.changes));
  await onWatchMode((message) => notify(message, "info"));
  // 設定（settings.toml）の警告/再読み込みの購読（要件10.3/10.4）。
  // 起動時破損・実行中の不完全保存・無効値の警告はすべて settings-warning で届く（backend が文言生成）。
  await onSettingsWarning((message) => notify(message, "warn"));
  // settings.toml の有効な編集保存を検知したら再読み込みし、可視4項目を即適用する（要件10.3 再起動なし反映）。
  // theme/wrap_default/tab_width はライブで反映する。default_mode は「初回既定」の意味なのでライブでは
  // 適用しない（initial:false で applySettings へ伝える＝現在開いているビューを強制切替しない）。
  await onSettingsChanged((next) => {
    state.settings = next;
    applySettings(next, { initial: false });
    notify("設定を再読み込みしました", "info");
  });
  // 起動直後に現在の有効設定を取得して保持し、可視4項目を初期適用する（取得失敗は握りつぶす＝設定が無くても起動を妨げない）。
  // applySettings(initial:true) は theme/wrap_default/tab_width に加え default_mode で viewMode の初期値を確定する。
  // restoreOnStartup（タブを開く）より前に走るので、復元前に viewMode が確定する。theme の確定もここで行い、
  // initTheme の暫定 system からの切替を最初の重い描画より前に済ませて FOUC（ちらつき）を避ける。
  try {
    state.settings = await getSettings();
    applySettings(state.settings, { initial: true });
  } catch {
    // 設定取得に失敗しても編集体験は継続（既定相当で動く）。
  }
  // 単一インスタンス転送（要件3.4）: 既存インスタンスへ後続プロセスが投げたパスを開く。
  await onOpenRequest((payload) => void onOpenRequestEvent(payload));
  // 終了直前にアプリ状態を保存（要件10.1。アトミック書込は backend）。デバウンス待ちでなく即時 flush。
  // 保留中のデバウンスタイマーを必ず先にクリアする（eval medium #30）。クリアしないと、終了直前の
  // 即時 flush 後にデバウンス分が後から発火し、二重保存や（終了で UI が消えた後の）未完走で
  // 終了直前保存が取りこぼされうる。タイマーを止めてから 1 回だけ確実に書く。
  window.addEventListener("beforeunload", () => {
    cancelPendingPersist();
    void persistAppStateNow();
  });
  // 起動時に state.json を復元（version 安全側・復元3分岐は backend が判定）。
  await restoreOnStartup();
  // 初回起動の引数オープン（要件3.2/3.4・Part 1）: pika 未起動でファイルをダブルクリックした初回プロセスは
  // 自分がサーバーで open-request イベントを受けない。起動引数を pull して開く（前回状態の復元の**後**に開く＝
  // 同じフォルダならタブを維持したまま指定ファイルを開く・要件3.2）。onOpenRequestEvent を転送経路と共用する。
  try {
    const startup = await takeStartupOpenRequest();
    if (startup) await onOpenRequestEvent(startup);
  } catch {
    // dev ブラウザ単体や command 不在時は無視（起動を妨げない）。
  }
  // Empty 3分岐の「フォルダ未オープン（no-folder）」文言（ui-design 15章）。
  if (!state.folder) setStatus(emptyMessage("no-folder"));
}

void main();
