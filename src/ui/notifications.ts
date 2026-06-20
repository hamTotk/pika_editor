// 通知バー（非モーダル）。最大3本＋「他N件」集約・優先順位・スコープ・自動消滅は
// sprint 7（design doc 19章 通知バーキュー運用）で本実装する。本スプリントは最小表示のみ。

export type NoticeKind = "info" | "warn" | "error";

const host = () => document.getElementById("notifications") as HTMLElement;

/** 通知を 1 本表示する（最小版）。 */
export function notify(message: string, kind: NoticeKind = "info"): void {
  const el = document.createElement("div");
  el.className = "notice";
  el.dataset.kind = kind;
  el.textContent = message;
  host().appendChild(el);
  // 暫定の自動消滅。種類別の消滅条件は sprint 7。
  window.setTimeout(() => el.remove(), 5000);
}
