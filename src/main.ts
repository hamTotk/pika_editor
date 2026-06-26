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
  restoreAppState,
  saveAppState,
  hashContent,
  noteRecent,
  showPreview,
  hidePreview,
  setPreviewBounds,
  logFolderPath,
  openInDefaultApp,
  openLogFolder,
  createEntry,
  deleteEntry,
  getSettings,
  onSettingsWarning,
  onSettingsChanged,
  imageInfo,
  assetUrl,
  type TreeEntry,
  type Settings,
  type OpenRequestPayload,
  type AppState,
  type DocEncoding,
  type LineEnding,
  type PreviewRect,
  type ImageInfo,
} from "./ipc";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { open as openNativeDialog } from "@tauri-apps/plugin-dialog";
import { initTheme, applyTheme, currentTheme, type ThemeMode } from "./theme";
import { initMenuBar, type MenuItemSpec, type MenuSpec } from "./ui/menu";
import { initA11y, announce } from "./a11y";
import { resolveShortcut, modsOf, normalizeKey, type Action, type Focus } from "./shortcuts";
import { renderTree, resetTreeExpansion, reloadTreeDir, pruneTreeDir } from "./ui/tree";
import { renderTabs, type TabModel } from "./ui/tabs";
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
   * 非テキスト（画像/非対応バイナリ＝要件12.2・U3）か。true のとき CM6 を作らず image-host で表示する
   * （画像は簡易ビュー、巨大画像/非対応は「既定アプリで開く」誘導）。モード/差分は無効・カーソルステータス無し。
   */
  nonText: boolean;
  /** 第2段階以降（編集不可・読み取り専用ビューア）か。開き直しメニューの無効化に使う（backend degrade.editing_off を保存）。 */
  editingOff: boolean;
  /**
   * 開いたときに検出した元エンコーディング（保存時に維持する＝要件5.2・eval medium）。
   * open_document が判定した値。これを save_document に渡し、Shift_JIS 等が暗黙に UTF-8 化
   * されるのを防ぐ（最上位原則「データを失わない」）。新規タブ/不明時は utf-8。
   */
  encoding: DocEncoding;
  /** 開いたときに BOM があったか（保存時に維持する＝要件5.2）。 */
  hasBom: boolean;
  /**
   * 開いたときに検出した改行コード（表示メニューの現在値表示用・要件5.2）。
   * 変換 backend は未実装のため表示専用（要件14章「足さない」）。新規/不明時は "none"。
   */
  lineEnding: LineEnding;
  /**
   * このタブで「許可して再読込」したオプトイン外部許可ホスト（要件6.2/6.3・2.4）。
   * 既定は undefined（外部遮断）。許可は **タブ単位** で保持し、別タブ/別文書では既定オフに戻る
   * （永続はしない＝要件6.2「既定は必ずオフに戻る」）。renderActivePreview がこれを buildPreview へ渡す。
   */
  allowExternal?: string[];
  /**
   * 未保存編集の保持バッファ（eval high #11・最上位原則「データを失わない」）。
   *
   * タブは単一の CM6（state.editor）を共有し、タブ切替のたびに editor を destroy→再生成する。
   * このため dirty タブから別タブへ切り替えると、退避先が無ければ編集中テキストが消える。さらに
   * 再アクティブ化時に activateTab が openDocument でディスク内容を読み直して上書きすると未保存編集を
   * 失う。これを防ぐため、**dirty タブから切り替える瞬間に編集中テキストをここへ退避**し、再アクティブ化
   * では（dirty なら）ディスクでなくこの draft を CM6 へ載せる。保存（onSave）成功で dirty=false に
   * なったら draft はクリアする（以後はディスク内容を正とする）。非 dirty タブは常に undefined。
   */
  draft?: string;
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

const workbench = () => document.getElementById("workbench") as HTMLElement;
const treeHeadLabel = () => document.getElementById("tree-head-label") as HTMLElement;
const treeCollapseBtn = () => document.getElementById("tree-collapse") as HTMLButtonElement;
const treeExpandBtn = () => document.getElementById("tree-expand") as HTMLButtonElement;
const editorPane = () => document.getElementById("editor-pane") as HTMLElement;
const editorHost = () => document.getElementById("editor-host") as HTMLElement;
const diffHost = () => document.getElementById("diff-host") as HTMLElement;
const previewHost = () => document.getElementById("preview-host") as HTMLElement;
// 非テキスト（画像/非対応バイナリ）の簡易ビュー占有領域（要件12.2・U3）。CM6 を作らずここで表示する。
const imageHost = () => document.getElementById("image-host") as HTMLElement;
// タブバー右端 tab-tools（UIブラッシュアップ T6 差分 C4）。モード切替セグメント・差分トグル・確認ボタン。
const modeButtons = () =>
  Array.from(
    document.querySelectorAll<HTMLButtonElement>("#tab-tools .seg button[data-mode]"),
  );
const toggleDiffBtn = () => document.getElementById("toggle-diff") as HTMLButtonElement;
const confirmBtn = () => document.getElementById("confirm-file") as HTMLButtonElement;
// 「ブラウザで開く」（tab-tools・UIブラッシュアップ T9）。アクティブタブのファイルを OS 既定アプリで開く。
const openExternalBtn = () => document.getElementById("open-external") as HTMLButtonElement;
// タブ列（横スクロールするコンテナ）。隠れ未読バッジ（T10）の可視判定に scrollLeft/clientWidth を読む。
const tabsEl = () => document.getElementById("tabs") as HTMLElement;
// 隠れた差分あり（未読）タブ数のバッジ（T10・差分 C5・ui-mock .hidden-unread）。
const hiddenUnreadBadge = () => document.getElementById("hidden-unread") as HTMLButtonElement;

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
  renderTabs(state.tabs, state.active, activateTab, state.unread, closeTab);
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

// ── ツリーのコンテキストメニュー＋新規作成/削除（要件11・design G・T5）─────────────────────

/** 現在開いている単一のコンテキストメニュー要素（多重表示を防ぐ）。 */
let openContext: HTMLElement | null = null;

/**
 * 開いているモーダル数（promptText・指摘5）。>0 の間はグローバルショートカット（window keydown）を
 * 無効化し、名前入力中の修飾キー操作（Ctrl+Shift+Enter＝確認済み / Ctrl+W＝タブを閉じる 等）が
 * 背景タブへ貫通するのを防ぐ。
 */
let modalDepth = 0;

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
  return p.replace(/\\/g, "/").toLowerCase();
}

/** TTL を過ぎた未消費の自作成抑制エントリを破棄する（stale 過剰一致＋無制限増加の防止・第2巡）。 */
function pruneSelfCreated(now: number): void {
  for (const [k, t] of selfCreatedPaths) {
    if (now - t > SELF_CREATED_TTL_MS) selfCreatedPaths.delete(k);
  }
}

/** コンテキストメニューを閉じる（外側クリック/Esc/ウィンドウblur/アクション後）。 */
function closeContextMenu(): void {
  if (openContext) {
    openContext.remove();
    openContext = null;
  }
  document.removeEventListener("pointerdown", onContextOutside, true);
  document.removeEventListener("keydown", onContextKey, true);
  window.removeEventListener("blur", closeContextMenu);
}

function onContextOutside(e: Event): void {
  if (openContext && !openContext.contains(e.target as Node)) closeContextMenu();
}
function onContextKey(e: KeyboardEvent): void {
  if (e.key === "Escape") {
    e.preventDefault();
    closeContextMenu();
  }
}

/** コンテキストメニュー 1 項目（"sep" は区切り線）。 */
type ContextItem = { label: string; danger?: boolean; run: () => void } | "sep";

/** 指定座標へコンテキストメニューを開く（共通土台・menu-pop の見た目を流用）。 */
function openContextMenu(items: ContextItem[], x: number, y: number): void {
  closeContextMenu();
  const menu = document.createElement("div");
  menu.className = "menu-pop context-menu";
  menu.setAttribute("role", "menu");
  for (const item of items) {
    if (item === "sep") {
      const sep = document.createElement("div");
      sep.className = "msep";
      menu.appendChild(sep);
      continue;
    }
    const row = document.createElement("div");
    row.className = item.danger ? "mrow danger" : "mrow";
    row.setAttribute("role", "menuitem");
    row.tabIndex = -1;
    const label = document.createElement("span");
    label.className = "mlabel";
    label.textContent = item.label;
    row.appendChild(label);
    row.addEventListener("click", () => {
      closeContextMenu();
      item.run();
    });
    menu.appendChild(row);
  }
  // 画面外へはみ出さないよう、仮表示で寸法を測ってから位置をクランプする。
  menu.style.position = "fixed";
  menu.style.visibility = "hidden";
  menu.style.zIndex = "2000";
  document.body.appendChild(menu);
  const rect = menu.getBoundingClientRect();
  const left = Math.max(4, Math.min(x, window.innerWidth - rect.width - 4));
  const top = Math.max(4, Math.min(y, window.innerHeight - rect.height - 4));
  menu.style.left = `${left}px`;
  menu.style.top = `${top}px`;
  menu.style.visibility = "visible";
  openContext = menu;
  // 外側クリック/Esc/blur で閉じる。トリガとなった contextmenu ジェスチャの後続イベントで
  // 即閉じないよう、登録は次フレームへ遅延する（capture フェーズで先取り）。
  setTimeout(() => {
    document.addEventListener("pointerdown", onContextOutside, true);
    document.addEventListener("keydown", onContextKey, true);
    window.addEventListener("blur", closeContextMenu);
  }, 0);
}

/**
 * テーマ準拠の自前プロンプトモーダル（名前入力・T5）。
 *
 * window.prompt はこのコードベースでは dev フォールバックでのみ使われ、Tauri 本番 WebView での動作
 * 実績が無い（無反応で新規作成が黙って失敗する事故を避ける）。window.confirm は本番実績があるので
 * 削除確認はそちらを使い、テキスト入力だけ自前モーダルにする。Promise で OK=入力値 / Cancel=null を返す。
 */
function promptText(message: string, placeholder = ""): Promise<string | null> {
  return new Promise((resolve) => {
    // 閉じた後にフォーカスを戻す元要素を退避（右クリックしたツリー行など・指摘10 a11y）。
    const prevFocus = document.activeElement as HTMLElement | null;
    const overlay = document.createElement("div");
    overlay.className = "modal-overlay";
    const box = document.createElement("div");
    box.className = "modal-box";
    box.setAttribute("role", "dialog");
    box.setAttribute("aria-modal", "true");
    const label = document.createElement("div");
    label.className = "modal-label";
    label.textContent = message;
    const input = document.createElement("input");
    input.type = "text";
    input.className = "modal-input";
    input.placeholder = placeholder;
    const actions = document.createElement("div");
    actions.className = "modal-actions";
    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.className = "modal-btn";
    cancel.textContent = "キャンセル";
    const ok = document.createElement("button");
    ok.type = "button";
    ok.className = "modal-btn primary";
    ok.textContent = "OK";
    actions.append(cancel, ok);
    box.append(label, input, actions);
    overlay.appendChild(box);
    document.body.appendChild(overlay);
    // モーダル表示中はグローバルショートカット（window keydown）を無効化する（指摘5: 名前入力中の
    // Ctrl+Shift+Enter→確認済み や Ctrl+W→タブを閉じる が背景タブへ貫通するのを防ぐ）。
    modalDepth += 1;
    input.focus();
    const close = (value: string | null): void => {
      overlay.remove();
      document.removeEventListener("keydown", onKey, true);
      modalDepth = Math.max(0, modalDepth - 1);
      // フォーカスを開く前の要素へ戻す（キーボード操作位置を見失わない・指摘10）。
      prevFocus?.focus?.();
      resolve(value);
    };
    // Tab フォーカスを box 内（input / キャンセル / OK）へ閉じ込める（指摘10 フォーカストラップ）。
    const focusables: HTMLElement[] = [input, cancel, ok];
    const onKey = (e: KeyboardEvent): void => {
      // 処理可否に関わらず、モーダル中のキーは背景（window ハンドラ）へ伝播させない（指摘5）。
      // ただし入力欄へは届かせる必要があるため capture では stopImmediatePropagation せず、
      // ここで stopPropagation して bubble 経路の window ハンドラだけを止める。
      if (e.key === "Escape") {
        e.preventDefault();
        e.stopPropagation();
        close(null);
      } else if (e.key === "Enter") {
        e.preventDefault();
        e.stopPropagation();
        close(input.value);
      } else if (e.key === "Tab") {
        // box 内の 3 要素を循環させる（外へ抜けない）。
        e.preventDefault();
        const active = document.activeElement as HTMLElement | null;
        const idx = focusables.indexOf(active as HTMLElement);
        const dir = e.shiftKey ? -1 : 1;
        const next = focusables[(idx + dir + focusables.length) % focusables.length];
        next?.focus();
      }
    };
    document.addEventListener("keydown", onKey, true);
    cancel.addEventListener("click", () => close(null));
    ok.addEventListener("click", () => close(input.value));
    // 背景（オーバーレイ自身）クリックでキャンセル（box 内クリックは透過しない）。
    overlay.addEventListener("pointerdown", (e) => {
      if (e.target === overlay) close(null);
    });
  });
}

/**
 * テーマ準拠の自前確認モーダル（はい/いいえ・T5 削除確認）。Promise で OK=true / Cancel=false を返す。
 *
 * 実機検証で window.confirm がこの Tauri/WebView2 ビルドでは**ダイアログを出さず即 true を返す**ことが
 * 判明したため（window.prompt と同根）、削除の確認をネイティブ confirm に頼らず自前モーダルにする
 * （誤クリックでの即削除を防ぐ＝最上位原則「データを失わない」。ごみ箱移動で復元可能だが確認は出す）。
 */
function confirmModal(
  message: string,
  opts?: { okLabel?: string; danger?: boolean },
): Promise<boolean> {
  return new Promise((resolve) => {
    const prevFocus = document.activeElement as HTMLElement | null;
    const overlay = document.createElement("div");
    overlay.className = "modal-overlay";
    const box = document.createElement("div");
    box.className = "modal-box";
    box.setAttribute("role", "dialog");
    box.setAttribute("aria-modal", "true");
    const label = document.createElement("div");
    label.className = "modal-label";
    // 複数行メッセージ（\n）は <br> で改行表示する。
    message.split("\n").forEach((line, i) => {
      if (i > 0) label.appendChild(document.createElement("br"));
      label.appendChild(document.createTextNode(line));
    });
    const actions = document.createElement("div");
    actions.className = "modal-actions";
    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.className = "modal-btn";
    cancel.textContent = "キャンセル";
    const ok = document.createElement("button");
    ok.type = "button";
    ok.className = opts?.danger ? "modal-btn danger-btn" : "modal-btn primary";
    ok.textContent = opts?.okLabel ?? "OK";
    actions.append(cancel, ok);
    box.append(label, actions);
    overlay.appendChild(box);
    document.body.appendChild(overlay);
    modalDepth += 1;
    ok.focus();
    const focusables: HTMLElement[] = [cancel, ok];
    const close = (value: boolean): void => {
      overlay.remove();
      document.removeEventListener("keydown", onKey, true);
      modalDepth = Math.max(0, modalDepth - 1);
      prevFocus?.focus?.();
      resolve(value);
    };
    const onKey = (e: KeyboardEvent): void => {
      if (e.key === "Escape") {
        e.preventDefault();
        e.stopPropagation();
        close(false);
      } else if (e.key === "Enter") {
        e.preventDefault();
        e.stopPropagation();
        close(true);
      } else if (e.key === "Tab") {
        e.preventDefault();
        const idx = focusables.indexOf(document.activeElement as HTMLElement);
        const dir = e.shiftKey ? -1 : 1;
        focusables[(idx + dir + focusables.length) % focusables.length]?.focus();
      }
    };
    document.addEventListener("keydown", onKey, true);
    cancel.addEventListener("click", () => close(false));
    ok.addEventListener("click", () => close(true));
    overlay.addEventListener("pointerdown", (e) => {
      if (e.target === overlay) close(false);
    });
  });
}

/** ツリー項目の右クリックメニュー（ファイル/フォルダ＝要件11）。 */
function showTreeContextMenu(entry: TreeEntry, x: number, y: number): void {
  // 新規作成先: フォルダ上ならその中、ファイル上ならその親フォルダ。
  const targetDir = entry.is_dir ? entry.path : parentDirOf(entry.path);
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

/** パスの親フォルダ（末尾区切り＋名前を落とす）。区切りが無ければ自身を返す。 */
function parentDirOf(path: string): string {
  const m = path.replace(/[\\/]+$/, "").replace(/[\\/][^\\/]+$/, "");
  return m || path;
}

/** パス区切り（\ と /）・末尾区切り・大小を吸収して等価比較する。 */
function samePath(a: string, b: string): boolean {
  const norm = (p: string): string => p.replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase();
  return norm(a) === norm(b);
}

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
    selfCreatedPaths.set(selfKey(created), Date.now());
    await refreshTreeDir(dir, { expand: expandDir });
    if (!isDir) {
      // 作成した空ファイルを開く（テキスト/非テキスト判定は openFile が拡張子で行う）。
      await openFile({ name: trimmed, path: created, is_dir: false });
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
    await refreshTreeDir(parentDirOf(entry.path));
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
      refreshTree();
    } catch {
      // 取得失敗は固めない（現状維持）。
    }
    return;
  }
  await reloadTreeDir(dir, opts);
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
    state.editor = createEditor(
      editorHost(),
      deletedContent,
      () => markDirty(path),
      () => refreshStatus(),
      state.lineWrapping,
      state.tabWidth,
    );
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
    state.editor = createEditor(
      editorHost(),
      content,
      () => markDirty(path),
      () => refreshStatus(),
      state.lineWrapping,
      state.tabWidth,
    );
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
  if (tab.dirty) {
    const ok = window.confirm(
      `「${tab.title}」には未保存の変更があります。\n` +
        `${encodingLabel(enc, false)} で開き直すと、未保存の変更は破棄されます。続けますか？`,
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
    state.editor = createEditor(
      editorHost(),
      doc.text,
      () => markDirty(path),
      () => refreshStatus(),
      state.lineWrapping,
      state.tabWidth,
    );
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

/** 3モード×差分トグルの直交占有を DOM へ適用する（要件6.1・ui-design 8章）。 */
function applyOccupancy(): void {
  // 非テキスト（画像/非対応バイナリ＝要件12.2・U3）が占有なら他の host を全て隠して image-host だけ出す。
  // 画像にモード/差分/プレビューは無いので resolveOccupancy より前に分岐して image-host を専有させる。
  const activeTab = state.tabs.find((t) => t.path === state.active);
  if (activeTab && activeTab.nonText && !activeTab.deleted) {
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
    return;
  }
  // 非画像占有（テキスト/差分/プレビュー）では image-host を必ず隠す（画像→テキスト切替の正規化）。
  imageHost().hidden = true;
  const occ = resolveOccupancy(state.viewMode, state.diffOn);
  // エディタが見えなくなる占有（差分ON/プレビューのみ）へ移ったら検索バーを閉じる（U4・要件5.4）。
  // 差分ビュー内検索・プレビュー内検索は系統C 繰り越しなので、ここで自然に無効化する（canSearch と整合）。
  if (!occ.showEditor && searchController?.isOpen()) searchController.close();
  editorHost().hidden = !occ.showEditor;
  diffHost().hidden = !occ.showDiff;
  previewHost().hidden = !occ.showPreview;
  // 左右並置（split）の出し分け（ui-design 8章）。occ は直交占有の結果なので、
  //  - プレビュー＋差分（preview/split で差分ON）→ 左＝レンダリング／右＝テキスト差分。
  //  - 分割＋差分OFF（split で showEditor && showPreview）→ 左＝エディタ／右＝プレビュー。
  //  - それ以外（単独占有）→ data-split を外し既存の単独レイアウト（grid-row:2 を 1 要素が占有）に戻す。
  if (occ.showPreview && occ.showDiff) {
    editorPane().setAttribute("data-split", "preview-diff");
  } else if (occ.showPreview && occ.showEditor) {
    editorPane().setAttribute("data-split", "editor-preview");
  } else {
    editorPane().removeAttribute("data-split");
  }
  // 占有（ソース/差分/プレビュー）が変わったらステータスのカーソル有無も切り替える（要件11.1）。
  // ソース占有のみカーソル位置を出し、差分/プレビュー占有では全体（差分・行・文字）のみにする。
  refreshStatus();
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
        // 未読解除・ツリー/タブのマーク解除（要件8.3）。
        state.unread.clearFile(path);
        refreshTree();
        refreshTabs();
        refreshStatus(); // 差分あり数（差分 N）を更新。
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
      refreshTree();
      refreshTabs();
      refreshStatus(); // 差分あり数（差分 N）を更新。
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
      refreshStatus(); // 差分あり数（差分 N）を更新（巻き戻しは内容も変わるので行/文字も再計測）。
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

/** アクティブタブのエンコーディング表記（表示メニューの現在値表示用・表示専用）。 */
function encodingLabel(enc: DocEncoding, hasBom: boolean): string {
  const base =
    enc === "utf-8"
      ? "UTF-8"
      : enc === "utf-16le"
        ? "UTF-16 LE"
        : enc === "utf-16be"
          ? "UTF-16 BE"
          : "Shift_JIS";
  return hasBom ? `${base} (BOM)` : base;
}

/** アクティブタブの改行コード表記（表示メニューの現在値表示用・表示専用）。 */
function lineEndingLabel(le: LineEnding): string {
  switch (le) {
    case "lf":
      return "LF";
    case "crlf":
      return "CRLF";
    case "cr":
      return "CR";
    case "mixed":
      return "混在";
    default:
      return "—";
  }
}

/**
 * 5つのメニュー（ファイル/編集/表示/移動/ヘルプ）の定義を組み立てる（UIブラッシュアップ T8）。
 * build は**開くたびに**評価され checked/disabled/現在値（エンコーディング/改行/テーマ/折り返し）を反映する。
 * 各項目は既存ハンドラへ結線する。backend 未実装（エンコーディング再オープン・改行変換・OS で開く）は
 * 項目を出さない or 案内に留める（要件14章「足さない」を厳守）。
 */
function buildMenuSpecs(): MenuSpec[] {
  const hasActive = (): boolean => !!state.active;
  const activeTab = (): OpenTab | undefined =>
    state.tabs.find((t) => t.path === state.active);
  // 「すべて確認済み」に対象があるか（無ければ無効化せず実行時に空案内する＝既存挙動を維持）。
  return [
    {
      id: "file",
      build: (): MenuItemSpec[] => [
        { kind: "item", label: "フォルダを開く…", accel: "Ctrl+Shift+O", onSelect: () => void onOpenFolder() },
        { kind: "item", label: "ファイルを開く…", accel: "Ctrl+O", onSelect: () => void onOpenFile() },
        { kind: "separator" },
        {
          kind: "item",
          label: "保存",
          accel: "Ctrl+S",
          disabled: !hasActive() || state.busy,
          onSelect: () => void onSave(),
        },
        { kind: "separator" },
        { kind: "item", label: "ログフォルダを開く", onSelect: () => void onOpenLogFolder() },
      ],
    },
    {
      id: "edit",
      build: (): MenuItemSpec[] => [
        {
          kind: "item",
          label: "すべて確認済み",
          accel: "Ctrl+Alt+Enter",
          disabled: state.busy,
          onSelect: () => void onConfirmAll(),
        },
        {
          kind: "item",
          label: "確認済み時点に戻す",
          disabled: !hasActive() || state.busy,
          onSelect: () => void onRollback(),
        },
        { kind: "separator" },
        {
          kind: "item",
          label: "検索",
          accel: "Ctrl+F",
          disabled: !hasActive(),
          onSelect: () => dispatchAction("find"),
        },
        {
          kind: "item",
          label: "置換",
          accel: "Ctrl+H",
          disabled: !hasActive(),
          onSelect: () => dispatchAction("replace"),
        },
      ],
    },
    {
      id: "view",
      build: (): MenuItemSpec[] => {
        const enabled = hasActive();
        const tab = activeTab();
        const rows: MenuItemSpec[] = [
          {
            kind: "item",
            label: "ソース",
            disabled: !enabled,
            checked: state.viewMode === "source",
            onSelect: () => void setViewMode("source"),
          },
          {
            kind: "item",
            label: "分割",
            accel: "Ctrl+\\",
            disabled: !enabled,
            checked: state.viewMode === "split",
            onSelect: () => void setViewMode("split"),
          },
          {
            kind: "item",
            label: "プレビュー",
            accel: "Ctrl+E",
            disabled: !enabled,
            checked: state.viewMode === "preview",
            onSelect: () => void setViewMode("preview"),
          },
          {
            kind: "item",
            label: "差分",
            accel: "Ctrl+Shift+D",
            disabled: !enabled,
            checked: state.diffOn,
            onSelect: () => void onToggleDiff(),
          },
          { kind: "separator" },
          {
            kind: "item",
            label: "折り返し",
            disabled: !enabled,
            checked: state.lineWrapping,
            onSelect: () => onToggleWrap(),
          },
          { kind: "separator" },
          // エンコーディングは「指定して開き直す」を実結線（要件5.6 Reopen）。改行コードは現在値の表示のみ
          // （変換 backend は未実装＝要件14章）。アクティブタブが無いときは出さない。
          ...(tab
            ? ([
                // エンコーディングを指定して開き直す（要件5.6 Reopen）。見出しに現在値を表示し、
                // 下の4項目で強制再デコードする。入れ子サブメニュー非対応のためフラット展開（テーマと同型）。
                {
                  kind: "item",
                  label: "エンコーディングを指定して開き直す",
                  accel: encodingLabel(tab.encoding, tab.hasBom),
                  disabled: true,
                },
                ...(["utf-8", "utf-16le", "utf-16be", "shift_jis"] as DocEncoding[]).map(
                  (e): MenuItemSpec => ({
                    kind: "item",
                    label: `　${encodingLabel(e, false)}`,
                    checked: tab.encoding === e,
                    // 削除済みタブは実体が無く開き直せない（backend verify_read も弾くが UI でも無効化）。
                    // 巨大ファイル（編集不可＝第2段階以降）も無効化（backend reopen 拒否と同条件）。
                    disabled: tab.deleted || tab.editingOff,
                    onSelect: () => void reopenActiveWithEncoding(e),
                  }),
                ),
                { kind: "separator" },
                {
                  kind: "item",
                  label: "改行コード",
                  accel: lineEndingLabel(tab.lineEnding),
                  disabled: true,
                },
                { kind: "separator" },
              ] as MenuItemSpec[])
            : []),
          {
            kind: "item",
            label: "テーマ: ライト",
            checked: currentTheme() === "light",
            onSelect: () => onSetTheme("light"),
          },
          {
            kind: "item",
            label: "テーマ: ダーク",
            checked: currentTheme() === "dark",
            onSelect: () => onSetTheme("dark"),
          },
          {
            kind: "item",
            label: "テーマ: システム",
            checked: currentTheme() === "system",
            onSelect: () => onSetTheme("system"),
          },
        ];
        return rows;
      },
    },
    {
      id: "go",
      build: (): MenuItemSpec[] => [
        {
          kind: "item",
          label: "次の変更",
          accel: "F8",
          disabled: !state.diff,
          onSelect: () => dispatchAction("next-change"),
        },
        {
          kind: "item",
          label: "前の変更",
          accel: "Shift+F8",
          disabled: !state.diff,
          onSelect: () => dispatchAction("prev-change"),
        },
        { kind: "separator" },
        { kind: "item", label: "再同期", accel: "F5", onSelect: () => void onF5() },
      ],
    },
    {
      id: "help",
      build: (): MenuItemSpec[] => [
        { kind: "item", label: "バージョン情報", onSelect: () => onShowVersion() },
      ],
    },
  ];
}

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
  // この経路は activateTab を通らず直接 editor を破棄/再生成する。切替前に現アクティブタブの未保存編集を
  // draft へ退避しておく（eval high #11・データを失わない）。退避漏れがあると、dirty タブがアクティブな
  // ときに「存在しないパス」を開くと editor 破棄で未保存編集が消え、戻っても draft===undefined で
  // 黙ってディスク内容を読み直してしまう。activateTab/collectAppState と同じ前置きで全経路を airtight にする。
  captureActivePosition();
  state.active = path;
  state.editor?.destroy();
  state.editor = createEditor(
    editorHost(),
    "",
    () => markDirty(path),
    () => refreshStatus(),
    state.lineWrapping,
    state.tabWidth,
  );
  void captureTabHash(path, "");
  refreshTabs();
  // 新規ファイルは「保存で作成される」旨を一旦提示し、以後の編集（onCursorChange）で構造化ステータスへ移る。
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
      // フォルダ名＋件数はツリーヘッダ（T3）へ移したのでステータスからは外す。タブ復元・活性化後に
      // refreshStatus（activateTab 経由）が構造化ステータスを描画する。
      setStatus("");
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
/**
 * 検索/置換バーを使えるビューか（U4/U5・要件5.4）。
 *
 * 条件: アクティブな**テキスト**タブがエディタとして見えていること。
 * - 画像/非対応バイナリ（nonText）・削除済み（deleted）・第2段階以降（editingOff）は CM6 が無い/編集不可。
 * - 差分ビューは CM6 でないため **差分内検索は系統C 繰り越し**（diffOn で showEditor が落ちると false）。
 *   プレビューのみは WebView2 の find に任せる想定で同じく系統C 繰り越し（要件5.4）。
 * → ソース／分割（エディタが可視）のときだけ true。
 */
function canSearch(): boolean {
  if (!state.active || !state.editor) return false;
  const tab = state.tabs.find((t) => t.path === state.active);
  if (!tab || tab.nonText || tab.deleted || tab.editingOff) return false;
  return resolveOccupancy(state.viewMode, state.diffOn).showEditor;
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
    case "find":
    case "replace": {
      // 検索/置換バーを開く（U4/U5・要件5.4）。差分ON/プレビュー/画像/第2段階では検索できない
      //（canSearch=false なら開かず案内のみ）。編集メニューの検索/置換もこの dispatchAction を呼ぶ。
      if (!canSearch()) {
        notify("このビューでは検索できません（ソース/分割で使えます）", "info");
        return true;
      }
      searchController?.open(action === "find" ? "find" : "replace");
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
  closeTab(state.active);
}

/**
 * 任意のタブを閉じる（Ctrl+W／タブの × クリック・要件11.2）。未保存があれば破棄確認を挟む
 * （データを失わない＝確認を必ず通す）。閉じたのがアクティブタブなら隣へアクティブを移す。
 */
function closeTab(path: string): void {
  const tab = state.tabs.find((t) => t.path === path);
  if (!tab) return;
  // タブを閉じるとエディタが作り直される/畳まれるので検索バーを閉じる（古いハイライトを残さない）。
  searchController?.close();
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
    state.editor = createEditor(
      editorHost(),
      "",
      () => undefined,
      undefined,
      state.lineWrapping,
      state.tabWidth,
    );
    // 直前が画像タブだった場合に image-host が残らないよう host 可視を正規化する（画像→空状態の正規化・U3）。
    // state.active=null なので applyOccupancy は image-host を隠し空エディタ（editor-host）を出す。
    applyOccupancy();
    refreshTabs();
    setStatus(emptyMessage("no-folder"));
  }
  void persistAppState();
}

async function main(): Promise<void> {
  initTheme();
  // ARIA 全Web再構築の初期化（F6/Shift+F6 ペイン間フォーカス循環・ランドマーク確実化＝要件11.5・design doc 17章）。
  initA11y();
  // カスタムメニューバー（UIブラッシュアップ T8）。ツールバー(#toolbar)は廃止し、フォルダを開く/保存/
  // すべて確認済み/巻き戻し/表示モード/折り返し/テーマ/再同期/バージョンを HTML/CSS/TS のメニューへ集約する。
  // 各メニューの活性・✓・現在値（エンコーディング/改行/テーマ/折り返し）は build が開くたびに評価する。
  const menubarEl = document.getElementById("menubar") as HTMLElement;
  const menuLayerEl = document.getElementById("menu-layer") as HTMLElement;
  initMenuBar(menubarEl, menuLayerEl, buildMenuSpecs());
  // フレームレス化の自前ウィンドウ操作（最小化/最大化/閉じる・ui-design §7）。ドラッグ移動は
  // メニュー帯の data-tauri-drag-region（index.html）が担う。
  initWindowControls();
  // 検索/置換バー（U4/U5・要件5.4）。deps は getter 経由で常に最新の state.editor / 内容を取る
  //（タブ切替で editor を作り直しても正しいエディタへ向く）。バー実体は #editor-pane の右上へ重ねる。
  searchController = createSearchController({
    editorPane: editorPane(),
    getEditor: () => state.editor,
    getContent: () => state.editor?.getContent() ?? null,
    canSearch,
    notify,
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
    // モーダル（名前入力プロンプト）表示中はグローバルショートカットを無効化する（指摘5）。
    // 名前入力中の Ctrl+Shift+Enter（確認済み）/ Ctrl+W（タブを閉じる）等が背景タブへ貫通しない。
    if (modalDepth > 0) return;
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
    if (persistTimer !== null) {
      window.clearTimeout(persistTimer);
      persistTimer = null;
    }
    void persistAppStateNow();
  });
  // 起動時に state.json を復元（version 安全側・復元3分岐は backend が判定）。
  await restoreOnStartup();
  // Empty 3分岐の「フォルダ未オープン（no-folder）」文言（ui-design 15章）。
  if (!state.folder) setStatus(emptyMessage("no-folder"));
}

void main();
