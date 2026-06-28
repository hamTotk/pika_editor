// メインWebView シェルの DOM 要素ゲッタ（旧 main.ts:192-216 を集約・S7）。
//
// index.html の固定 ID 要素を型付きで取り出す薄いゲッタ群。取得対象・キャスト（null 非保証）は
// 旧実装と不変（純粋リファクタ）。レイアウト要素は遅延（毎回 getElementById）で取り、初期化順序に
// 依存しない（DOM は index.html に静的に存在する）。

/** ワークベンチ全体（grid のルート）。 */
export const workbench = (): HTMLElement => document.getElementById("workbench") as HTMLElement;
/** ツリーヘッダのラベル（フォルダ名表示）。 */
export const treeHeadLabel = (): HTMLElement => document.getElementById("tree-head-label") as HTMLElement;
/** ツリーを全折りたたみするボタン。 */
export const treeCollapseBtn = (): HTMLButtonElement => document.getElementById("tree-collapse") as HTMLButtonElement;
/** ツリーを全展開するボタン。 */
export const treeExpandBtn = (): HTMLButtonElement => document.getElementById("tree-expand") as HTMLButtonElement;
/** エディタペイン（検索バーを重ねる土台・position:relative）。 */
export const editorPane = (): HTMLElement => document.getElementById("editor-pane") as HTMLElement;
/** CM6 を載せるホスト。 */
export const editorHost = (): HTMLElement => document.getElementById("editor-host") as HTMLElement;
/** 差分ビューのホスト。 */
export const diffHost = (): HTMLElement => document.getElementById("diff-host") as HTMLElement;
/** プレビュー別WebView を重ねる矩形ホスト。 */
export const previewHost = (): HTMLElement => document.getElementById("preview-host") as HTMLElement;
/** 非テキスト（画像/非対応バイナリ）の簡易ビュー占有領域（要件12.2・U3）。 */
export const imageHost = (): HTMLElement => document.getElementById("image-host") as HTMLElement;
/** tab-tools のモード切替セグメント（data-mode のボタン群）。 */
export const modeButtons = (): HTMLButtonElement[] =>
  Array.from(document.querySelectorAll<HTMLButtonElement>("#tab-tools .seg button[data-mode]"));
/** 差分トグルボタン。 */
export const toggleDiffBtn = (): HTMLButtonElement => document.getElementById("toggle-diff") as HTMLButtonElement;
/** 「確認済みにする」ボタン。 */
export const confirmBtn = (): HTMLButtonElement => document.getElementById("confirm-file") as HTMLButtonElement;
/** 「ブラウザで開く」ボタン。 */
export const openExternalBtn = (): HTMLButtonElement => document.getElementById("open-external") as HTMLButtonElement;
/** 全タブ一覧ドロップダウンのトリガ。 */
export const tabListBtn = (): HTMLButtonElement => document.getElementById("tab-list") as HTMLButtonElement;
/** タブ列（横スクロールするコンテナ）。 */
export const tabsEl = (): HTMLElement => document.getElementById("tabs") as HTMLElement;
/** 隠れた差分あり（未読）タブ数のバッジ。 */
export const hiddenUnreadBadge = (): HTMLButtonElement => document.getElementById("hidden-unread") as HTMLButtonElement;
