// ステータス（右下固定・表示専用・pointer-events:none＝ui-design 9章）。
// design doc 17章: 表示専用だが差分あり（未読）件数を aria-label でナレーターへ供給する（要件11.5）。
// 内容は 差分あり件数・行数・文字数・カーソル位置（要件11.1）。視覚は CSS（右下固定・pointer-events:none）。

const host = () => document.getElementById("status") as HTMLElement;

/** ステータスの構成要素（要件11.1）。 */
export interface StatusInfo {
  /** 差分あり（未読）ファイル数（要件11.1・design doc 17章で aria-label 化）。 */
  unreadCount?: number;
  /** 行数。 */
  lines?: number;
  /** 文字数。 */
  chars?: number;
  /** カーソル位置（1 始まり）。 */
  cursorLine?: number;
  cursorColumn?: number;
  /**
   * 文字選択中の選択文字数（>0 のとき）。指定されるとカーソル位置の代わりに
   * 「選択 N 文字」を出す（ui-design 9章）。
   */
  selectionChars?: number;
}

/** ステータス行を自由テキストで設定する（Empty/新規/削除済み/エラー等の状況メッセージ用）。 */
export function setStatus(text: string): void {
  // 構造化セグメント（span）を捨てて素のテキストに戻す。aria-label も外す（自由文は textContent が読まれる）。
  const el = host();
  el.replaceChildren();
  el.textContent = text;
  el.removeAttribute("aria-label");
}

/** ステータスの各セグメントを `<span class="seg-s">` で組む（ui-mock .status .seg-s）。 */
function seg(text: string, extraClass?: string): HTMLSpanElement {
  const s = document.createElement("span");
  s.className = extraClass ? `seg-s ${extraClass}` : "seg-s";
  s.textContent = text;
  return s;
}

/**
 * 構造化ステータスを描画し、差分あり件数を aria-label に載せる（design doc 17章・要件11.5・ui-mock）。
 *
 * フォーマット（ui-mock .status の DOM 構成）:
 *   `<span class="seg-s">差分 N</span>` 全体＝text-2
 *   `<span class="seg-s">N 行</span>`
 *   `<span class="seg-s">N 文字</span>`（toLocaleString で桁区切り）
 *   `<span class="seg-s cursor">行 N, M 文字目</span>` カーソルは accent 色（.status .cursor）
 * 文字選択中は cursor span を「選択 N 文字」に差し替える。カーソルが無いモード
 * （プレビュー/差分）は cursorLine/Column を渡さない＝全体（差分/行/文字）のみ表示する。
 */
export function renderStatus(info: StatusInfo): void {
  const el = host();
  el.replaceChildren();
  if (info.unreadCount !== undefined) el.appendChild(seg(`差分 ${info.unreadCount}`));
  if (info.lines !== undefined) el.appendChild(seg(`${info.lines} 行`));
  if (info.chars !== undefined) el.appendChild(seg(`${info.chars.toLocaleString()} 文字`));
  // 選択中は「選択 N 文字」、それ以外でカーソル位置があれば「行 N, M 文字目」を accent 色で出す。
  if (info.selectionChars !== undefined && info.selectionChars > 0) {
    el.appendChild(seg(`選択 ${info.selectionChars.toLocaleString()} 文字`, "cursor"));
  } else if (info.cursorLine !== undefined && info.cursorColumn !== undefined) {
    el.appendChild(seg(`行 ${info.cursorLine}, ${info.cursorColumn} 文字目`, "cursor"));
  }
  // 表示専用（pointer-events:none）だがナレーターには差分あり件数を読ませる（要件11.5）。
  if (info.unreadCount !== undefined) {
    el.setAttribute("aria-label", `差分あり ${info.unreadCount} 件`);
  } else {
    el.removeAttribute("aria-label");
  }
}
