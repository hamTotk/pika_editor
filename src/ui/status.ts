// ステータス（右下固定・表示専用・pointer-events:none）。
// 差分あり件数の aria-label 化は sprint 7（design doc 17章）。本スプリントは文言表示のみ。

const host = () => document.getElementById("status") as HTMLElement;

/** ステータス行の文言を設定する。 */
export function setStatus(text: string): void {
  host().textContent = text;
}
