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
}

/** ステータス行を設定する（差分あり件数を aria-label でも供給する＝要件11.5）。 */
export function setStatus(text: string): void {
  host().textContent = text;
}

/** 構造化ステータスを描画し、差分あり件数を aria-label に載せる（design doc 17章・要件11.5）。 */
export function renderStatus(info: StatusInfo): void {
  const parts: string[] = [];
  if (info.unreadCount !== undefined) parts.push(`差分あり ${info.unreadCount}`);
  if (info.lines !== undefined) parts.push(`${info.lines} 行`);
  if (info.chars !== undefined) parts.push(`${info.chars} 文字`);
  if (info.cursorLine !== undefined && info.cursorColumn !== undefined) {
    parts.push(`${info.cursorLine}:${info.cursorColumn}`);
  }
  const el = host();
  el.textContent = parts.join("  ");
  // 表示専用（pointer-events:none）だがナレーターには差分あり件数を読ませる（要件11.5）。
  if (info.unreadCount !== undefined) {
    el.setAttribute("aria-label", `差分あり ${info.unreadCount} 件`);
  }
}
