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

import { preparePreview, scanHtmlHazards, type PreviewMode } from "../ipc";

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
}

/**
 * プレビューを準備して別WebView へ流す URL を得る（要件6・design doc 6章）。
 *
 * `content` は編集バッファ or ディスク内容。backend はこれを comrak→ammonia でサニタイズし
 * custom protocol のキャッシュに置く。**HTML 本体は戻り値に乗らない**（URL のみ）。
 */
export async function buildPreview(
  path: string,
  content: string,
  opts: { allowExternal?: string[] } = {},
): Promise<PreviewReady> {
  const mode = previewModeForPath(path);
  const prepared = await preparePreview(path, mode, content, opts.allowExternal);
  return {
    url: prepared.url,
    generation: prepared.generation,
    nonce: prepared.nonce,
    flavor: prepared.flavor,
  };
}

/**
 * 系統B（HTML）プレビューの危険検知（要件6.3）。
 * `<script>` や外部参照・`<meta refresh>` を検知したら通知バーで「既定のブラウザで開く」へ誘導する。
 */
export async function detectHtmlHazards(content: string): Promise<{
  hasScript: boolean;
  hasExternalRef: boolean;
  hasMetaRefresh: boolean;
}> {
  const h = await scanHtmlHazards(content);
  return {
    hasScript: h.has_script,
    hasExternalRef: h.has_external_ref,
    hasMetaRefresh: h.has_meta_refresh,
  };
}

/**
 * 別WebView へ注入する信頼 JS の初期化スクリプトを組み立てる（系統A・要件6.2・design doc 6章）。
 *
 * 危険オプション封じ（design doc 6章「信頼済み JS の危険オプション封じ」）:
 * - **Mermaid**: `securityLevel:'strict'`（click/任意 HTML 生成禁止）・`startOnLoad:false`（手動 per-block 描画）。
 * - **KaTeX**: `trust:false`・`strict:true`・`maxExpand` 制限（`\href{javascript:}`/`\htmlData` 経路封じ）。
 * - **highlight.js**: コードブロックのみ。
 * - 各ブロック個別描画。構文エラー/タイムアウト（約1秒）で当該ブロックを元コード表示へ戻しエラーマーク。
 * - 失敗件数は `postMessage` でメインWebView へ通知（通知バー集計＝要件6.2）。
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
    `  function reportFailures(){`,
    `    if (failures > 0 && window.parent !== window) {`,
    // 失敗件数のみ通知（内容は送らない＝オリジン分離・通知バー集計）。
    `      try { window.parent.postMessage({ type: "pika-preview-failures", count: failures }, "*"); } catch(e){}`,
    `    }`,
    `  }`,
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
