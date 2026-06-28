// プレビュー（権限ゼロ別WebView＋custom protocol 直配信）のフロント制御（要件6・design doc 6章）。
//
// 本モジュールはメインWebView 側で動く。プレビュー HTML 本体は invoke で受け取らず（IPC 予算＋
// オリジン分離）、backend の prepare_preview が返す URL を別WebView の src に設定するだけ（design doc 6章）。
// HTML は JS のメインワールドを一切経由しない。
//
// 本モジュールが担うのは:
// - 3モード（ソース/分割/プレビューのみ）× 差分トグルの直交状態と占有世代（ui-design 8章・design doc 6章）。
// - 系統A/B 切替の直列化（世代カウンタ＋load 完了待ち＋破棄順序で前モード残留を防ぐ）。
// - 信頼 JS（Mermaid/KaTeX/highlight）の危険オプション封じと nonce 付与（design doc 6章）。
//   ※ 実際の注入先は別WebView。本モジュールは「注入する信頼スクリプト文字列の生成」を担い、
//      各ブロック個別描画・構文エラー/タイムアウト時の元コード復帰・失敗件数集計を実装する。

import {
  preparePreview,
  type PreparedPreview,
  type PreviewMode,
  type PreviewTheme,
  type ViewMode,
} from "../ipc";

/**
 * 表示モード（要件6.1）。差分トグルは別の直交フラグ（diffOn）。
 * 定義は `../ipc`（state.json の ViewMode と対応）に一元化し、ここでは再エクスポートのみ行う。
 */
export type { ViewMode };

/** 占有世代（タブ,モード,差分）。切替直列化に使う（design doc 6章・ui-design 8章）。 */
export interface PreviewGeneration {
  /** 対象タブのパス。 */
  path: string;
  /** 表示モード。 */
  mode: ViewMode;
  /** 差分トグル。 */
  diffOn: boolean;
}

/**
 * 系統A/B 切替の直列化マネージャ（design doc 6章「世代カウンタ＋load 完了待ち＋破棄順序」）。
 *
 * タブ/モード/差分を素早く切替えた際、古い prepare_preview の結果が後から到着して前モードが
 * 残留するのを防ぐ。各要求に単調増加の世代番号を振り、**最新世代の結果のみ採用**する。
 */
export class PreviewSerializer {
  private current = 0;

  /** 新しい占有世代を発番する（切替のたびに呼ぶ）。 */
  next(): number {
    this.current += 1;
    return this.current;
  }

  /** 与えられた世代が最新（採用してよい）か。古ければ破棄する（前モード残留防止）。 */
  isCurrent(generation: number): boolean {
    return generation === this.current;
  }
}

/**
 * 文書の系統（A/B）をファイル種別から決める（要件6.1/6.2/6.3）。
 * - `.md` / `.markdown` → 系統A（Markdown・信頼 JS）
 * - `.html` / `.htm` → 系統B（HTML・JS 無効）
 * - `.svg` → 系統A（SVG は信頼描画・サニタイズ済み）
 */
export function previewModeForPath(path: string): PreviewMode {
  const lower = path.toLowerCase();
  if (lower.endsWith(".html") || lower.endsWith(".htm")) return "html";
  // .md/.markdown/.svg は系統A（Markdown レンダラ経由・信頼 JS 注入対象）。
  return "markdown";
}

/**
 * メインアプリの解決済みトークン色から別WebView 用テーマを組む（Stage ③・design doc 10章）。
 *
 * DOM へ依存しない純粋関数にして単体テスト可能にする。`getToken("--bg-raised")` のような
 * 「CSS 変数の解決済み値（trim 済み文字列）を返す関数」と「ダークか」を受け、[`PreviewTheme`] を返す。
 * トークンが空（未解決）のときは空文字のまま渡す（backend が安全化検証で fail-closed に倒す）。
 *
 * 呼び出し側（main.ts）は `getComputedStyle(document.documentElement).getPropertyValue(name).trim()` を
 * `getToken` に渡し、`colorScheme` に "dark" を含むか / `matchMedia('(prefers-color-scheme: dark)')` と
 * data-theme から `dark` を解決する。
 */
export function resolvePreviewTheme(
  getToken: (name: string) => string,
  dark: boolean,
): PreviewTheme {
  return {
    bg: getToken("--bg-raised"),
    fg: getToken("--text-1"),
    muted: getToken("--text-2"),
    border: getToken("--border-2"),
    accent: getToken("--accent"),
    sunken: getToken("--bg-sunken"),
    dark,
  };
}

/**
 * プレビュー準備の結果（別WebView へ流すための情報）。
 * backend の prepare_preview 戻り（`PreparedPreview`）と構造同一なので別定義を持たず再エクスポートで一元化する
 * （url / generation / nonce / flavor / hazards）。
 */
export type PreviewReady = PreparedPreview;

/**
 * プレビューを準備して別WebView へ流す URL を得る（要件6・design doc 6章）。
 *
 * `content` は編集バッファ or ディスク内容。backend はこれを comrak→ammonia でサニタイズし
 * custom protocol のキャッシュに置く。**HTML 本体は戻り値に乗らない**（URL のみ）。
 * 系統B の危険検知（要件6.3）も同じ 1 回の invoke の戻り（hazards）で受け取る（IPC 二重転送回避）。
 *
 * `opts.theme`（Stage ③・design doc 10章）はメインアプリの解決済みトークン色。別WebView は独立文書で
 * CSS 変数を継承しないため backend へ渡し `:root{--pk-*}` として注入させる。系統B（HTML）は文書スタイル
 * 尊重で backend が theme を無視する（要件11.3）。色文字列の安全化検証は backend(pika-core)が担う。
 */
export async function buildPreview(
  path: string,
  content: string,
  opts: { allowExternal?: string[]; theme?: PreviewTheme } = {},
): Promise<PreviewReady> {
  const mode = previewModeForPath(path);
  // PreparedPreview と PreviewReady は構造同一のためフィールド写しをやめてそのまま返す（URL のみ・HTML 本体は非経由）。
  return preparePreview(path, mode, content, opts.allowExternal, opts.theme);
}

// 信頼 JS（Mermaid/KaTeX/highlight.js）の初期化スクリプトの**実行時の正本は Rust 側**
// `crates/pika-core/src/render/mod.rs` の `TRUSTED_JS_INIT`（`wrap_preview_document` が別WebView 文書へ
// inline 注入する）。信頼 JS と同梱アセットは custom protocol 経由で別WebView 内に閉じ、メイン WebView の
// JS ワールドを一切経由しない（HTML/JS を invoke で返さない＝design doc 6章）。かつてここに同等処理を
// 生成する `buildTrustedJsInit` の TS 写しがあったが、ランタイム未使用でドリフト源になるため削除した。
// 危険オプション封じ（Mermaid securityLevel:strict / KaTeX trust:false・strict:true・maxExpand 制限 /
// 各ブロック個別描画・約1秒タイムアウトで元コードへ復帰）は Rust 側 `TRUSTED_JS_INIT` を正本とする。

/**
 * モード切替の占有解決（要件6.1・ui-design 8章「3モード×差分トグル直交」）。
 *
 * - プレビュー + 差分ON は自動で分割相当（左=テキスト差分・右=レンダリング・修正5 要件改定）に倒す（要件6.1）。
 * - ソース + 差分ON はエディタ領域に差分面を重ねる。
 * - プレビューのみ/分割でプレビュー WebView を占有する。
 *
 * 戻り値は「プレビュー WebView を表示すべきか」「差分面を表示すべきか」。
 */
export function resolveOccupancy(mode: ViewMode, diffOn: boolean): {
  showPreview: boolean;
  showEditor: boolean;
  showDiff: boolean;
} {
  if (mode === "preview") {
    // プレビュー + 差分ON は分割相当（左テキスト差分・右レンダリング・修正5）。
    return { showPreview: true, showEditor: false, showDiff: diffOn };
  }
  if (mode === "split") {
    return { showPreview: true, showEditor: !diffOn, showDiff: diffOn };
  }
  // source
  return { showPreview: false, showEditor: !diffOn, showDiff: diffOn };
}
