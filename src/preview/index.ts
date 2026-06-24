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
  type HtmlHazards,
  type PreviewMode,
  type PreviewTheme,
} from "../ipc";

/** 表示モード（要件6.1）。差分トグルは別の直交フラグ（diffOn）。 */
export type ViewMode = "source" | "split" | "preview";

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

/** プレビュー準備の結果（別WebView へ流すための情報）。 */
export interface PreviewReady {
  /** 別WebView へナビゲートする URL（pika-preview:// 経由）。 */
  url: string;
  /** 占有世代（backend 側の世代。古ければ破棄）。 */
  generation: number;
  /** 系統A の信頼 JS 注入に使う nonce（系統B では空）。 */
  nonce: string;
  /** 系統（"markdown" | "html"）。 */
  flavor: string;
  /** 系統B の危険検知（要件6.3・通知バー導線）。系統A は全 false。 */
  hazards: HtmlHazards;
}

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
  const prepared = await preparePreview(path, mode, content, opts.allowExternal, opts.theme);
  return {
    url: prepared.url,
    generation: prepared.generation,
    nonce: prepared.nonce,
    flavor: prepared.flavor,
    hazards: prepared.hazards,
  };
}

/**
 * 別WebView へ注入する信頼 JS の初期化スクリプトを組み立てる（系統A・要件6.2・design doc 6章）。
 *
 * **【Stage ② で実行時の正本は Rust 側へ移動】** 注入する信頼 JS の正本は
 * `crates/pika-core/src/render/mod.rs` の `TRUSTED_JS_INIT`（`wrap_preview_document` が inline 注入）。
 * 信頼 JS と同梱アセットは custom protocol 経由で別WebView 内に閉じ、メイン WebView の JS ワールドを
 * 一切経由しない（HTML/JS を invoke で返さない＝design doc 6章）。本関数はもはやランタイムでは呼ばれず、
 * **意味の対照参照として残す**（変更時は Rust 側 `TRUSTED_JS_INIT` を正とし、文字列の意味を一致させること）。
 *
 * **【Stage ③・失敗の可視化】** 描画失敗は **in-preview で可視化する**（失敗ブロックへ `pika-block-error` を
 * 付け、別WebView 文書の BASE_CSS が縦線＋「描画に失敗」ラベルで視認可能にする＝要件6.2）。
 * F-029 でプレビュー別WebView からの IPC/event は境界で全拒否されるため、失敗件数をメインへ送る
 * 経路は持たない（プレビューに IPC 穴を開け直さない）。旧 `postMessage` 経由の失敗件数通知・
 * `parsePreviewFailureMessage`・main.ts の message リスナは廃止した（送信先が無いため送らない）。
 *
 * 危険オプション封じ（design doc 6章「信頼済み JS の危険オプション封じ」）:
 * - **Mermaid**: `securityLevel:'strict'`（click/任意 HTML 生成禁止）・`startOnLoad:false`（手動 per-block 描画）。
 * - **KaTeX**: `trust:false`・`strict:true`・`maxExpand` 制限（`\href{javascript:}`/`\htmlData` 経路封じ）。
 * - **highlight.js**: コードブロックのみ。
 * - 各ブロック個別描画。構文エラー/タイムアウト（約1秒）で当該ブロックを元コード表示へ戻しエラーマーク。
 *
 * 返すのは別WebView の `<script nonce="...">` に入れる本文（インライン）。`'unsafe-inline'`(script) は
 * CSP に付けず nonce のみ（design doc 6章）。
 */
export function buildTrustedJsInit(nonce: string): string {
  // nonce はサニタイズ済み（base64url・backend 生成）だが、文字列連結の安全のため英数記号のみ採用する。
  const safeNonce = nonce.replace(/[^A-Za-z0-9_-]/g, "");
  // 別WebView 内で動く初期化（per-block 描画・約1秒タイムアウト・失敗件数集計）。
  return [
    `(function(){`,
    `  "use strict";`,
    `  var BLOCK_TIMEOUT_MS = 1000;`,
    `  var failures = 0;`,
    // 失敗件数の集計は per-block 描画の制御に使う（in-preview で可視化）。メインへ送る経路は
    // 持たない（F-029 で別WebView の IPC/event は全拒否。targetOrigin "*" の postMessage は廃止）。
    `  function reportFailures(){ /* in-preview 可視化のみ。外部送信なし。 */ }`,
    // --- highlight.js（コードブロック） ---
    `  function runHighlight(){`,
    `    if (!window.hljs) return;`,
    `    document.querySelectorAll("pre code").forEach(function(el){`,
    `      try { window.hljs.highlightElement(el); } catch(e){ failures++; }`,
    `    });`,
    `  }`,
    // --- KaTeX（数式・trust:false/strict:true/maxExpand 制限） ---
    `  function runKatex(){`,
    `    if (!window.renderMathInElement) return;`,
    `    try {`,
    `      window.renderMathInElement(document.body, {`,
    `        delimiters: [`,
    `          { left: "$$", right: "$$", display: true },`,
    `          { left: "$", right: "$", display: false },`,
    `          { left: "\\\\(", right: "\\\\)", display: false },`,
    `          { left: "\\\\[", right: "\\\\]", display: true }`,
    `        ],`,
    // 危険オプション封じ（要件6.2・design doc 6章）。
    `        trust: false, strict: true, maxExpand: 1000, throwOnError: false,`,
    `        errorCallback: function(){ failures++; }`,
    `      });`,
    `    } catch(e){ failures++; }`,
    `  }`,
    // --- Mermaid（図・securityLevel:strict・per-block 描画・約1秒タイムアウト） ---
    `  function runMermaid(){`,
    `    if (!window.mermaid) return;`,
    `    try { window.mermaid.initialize({ startOnLoad: false, securityLevel: "strict" }); } catch(e){}`,
    `    var blocks = document.querySelectorAll("pre code.language-mermaid, code.language-mermaid");`,
    `    blocks.forEach(function(el, i){`,
    `      var src = el.textContent || "";`,
    `      var host = document.createElement("div");`,
    `      host.className = "pika-mermaid";`,
    `      var id = "pika-mmd-" + i;`,
    `      var done = false;`,
    `      var timer = setTimeout(function(){ if(!done){ done = true; failures++; markError(el); } }, BLOCK_TIMEOUT_MS);`,
    `      try {`,
    `        window.mermaid.render(id, src).then(function(out){`,
    `          if (done) return; done = true; clearTimeout(timer);`,
    `          host.innerHTML = out.svg;`,
    `          var pre = el.closest("pre") || el;`,
    `          pre.parentNode.replaceChild(host, pre);`,
    `        }).catch(function(){ if(done) return; done = true; clearTimeout(timer); failures++; markError(el); });`,
    `      } catch(e){ if(!done){ done = true; clearTimeout(timer); failures++; markError(el); } }`,
    `    });`,
    `  }`,
    // 構文エラー/タイムアウト時は当該ブロックを元コード表示へ戻しエラーマーク（要件6.2）。
    `  function markError(el){`,
    `    var pre = el.closest("pre") || el;`,
    `    if (pre && pre.classList) pre.classList.add("pika-block-error");`,
    `  }`,
    `  function init(){ runHighlight(); runKatex(); runMermaid(); setTimeout(reportFailures, BLOCK_TIMEOUT_MS + 50); }`,
    `  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);`,
    `  else init();`,
    `})();`,
    `// nonce=${safeNonce}`,
  ].join("\n");
}

/**
 * モード切替の占有解決（要件6.1・ui-design 8章「3モード×差分トグル直交」）。
 *
 * - プレビュー + 差分ON は自動で分割相当（左=レンダリング・右=テキスト差分）に倒す（要件6.1・spec 非対象でない）。
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
    // プレビュー + 差分ON は分割相当（左レンダリング・右テキスト差分）。
    return { showPreview: true, showEditor: false, showDiff: diffOn };
  }
  if (mode === "split") {
    return { showPreview: true, showEditor: !diffOn, showDiff: diffOn };
  }
  // source
  return { showPreview: false, showEditor: !diffOn, showDiff: diffOn };
}
