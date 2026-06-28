// 通知バー（非モーダル・要件11.1）。最大3本＋「他N件」集約・優先順位・スコープ・自動消滅を
// pika-core::notify_queue の決定論モデルと同じ規則でフロント側にも持つ（DOM 描画は系統C で検証）。
//
// 要件11.1 のキュー運用:
// - 同時表示は最大3本。超過分は「他N件」へ集約する。
// - 優先順位 衝突 ＞ 設定エラー ＞ 外部リソース ＞ JS検知 ＞ 巨大ファイル制限（＞ ファイル削除）。
// - 同一ファイル・同一種別は最新へ集約（合体）。
// - タブ固有はアクティブタブで切り替わり、グローバルは常時表示。
// - 種類ごとの自動消滅条件（解消で消える／閉じるまで残る）。
//
// 通知バーは role="status" / aria-live="polite"（非モーダル通知の読み上げ・design doc 17章）。

/** 通知の種別（pika-core::notify_queue::NoticeKind と対応・要件11.1）。 */
export type NoticeKind =
  | "conflict"
  | "settings-error"
  | "external-resource"
  | "script-detected"
  | "huge-file-limit"
  | "file-removed";

/** 種別の表示優先順位（小さいほど先・要件11.1）。 */
const PRIORITY: Record<NoticeKind, number> = {
  conflict: 0,
  "settings-error": 1,
  "external-resource": 2,
  "script-detected": 3,
  "huge-file-limit": 4,
  "file-removed": 5,
};

const MAX_VISIBLE = 3;

interface Notice {
  kind: NoticeKind;
  /** タブ固有ならパス、グローバルなら null。 */
  path: string | null;
  message: string;
  seq: number;
  /** 任意のアクションボタン（［差分を見る］［既定のブラウザで開く］等・要件11.1）。 */
  actions?: { label: string; onClick: () => void }[];
}

/** 同一ファイル・同一種別を合体する集約キー。 */
function mergeKey(kind: NoticeKind, path: string | null): string {
  return `${kind}::${path ?? "*global*"}`;
}

/**
 * 簡易トーストの重大度（旧 notify(message, level) のレベル）。
 * 上の NoticeKind（pika-core::notify_queue の写し＝衝突/設定エラー等の**意味のある**通知）とは別物で、
 * フロント限定の **使い捨て（transient）** 通知。一定時間で自動消滅し、意味付き通知の優先順位/合体/
 * 解消ロジックに混ぜない（eval medium #25: error→conflict 化け・同種一括 dismiss の取り違えを断つ）。
 */
export type ToastLevel = "info" | "warn" | "error";

/** 使い捨てトースト 1 件。seq で「その 1 件だけ」を消すため自動消滅は seq 指定で行う。 */
interface Toast {
  level: ToastLevel;
  message: string;
  seq: number;
}

const host = () => document.getElementById("notifications") as HTMLElement;

/** 通知キュー（決定論モデル・pika-core::notify_queue と同規則）。 */
class NoticeQueue {
  private items: Notice[] = [];
  private nextSeq = 0;
  private activeTab: string | null = null;
  /** 使い捨てトースト（簡易 notify 用・意味付き通知とは独立。自動消滅は seq 指定で 1 件ずつ）。 */
  private toasts: Toast[] = [];
  /** トースト seq の採番（意味付き通知の nextSeq とは分けて衝突させない）。 */
  private toastSeq = 0;
  /** 同時表示するトーストの上限（古いものから捨てる。自動消滅もあるので通常は溜まらない）。 */
  private static readonly MAX_TOASTS = 4;

  /** アクティブタブを設定する（タブ切替で表示が切り替わる・要件11.1）。 */
  setActiveTab(path: string | null): void {
    this.activeTab = path;
    this.render();
  }

  /** 通知を積む（同一ファイル・同一種別は最新へ合体）。 */
  push(
    kind: NoticeKind,
    path: string | null,
    message: string,
    actions?: { label: string; onClick: () => void }[],
  ): void {
    const key = mergeKey(kind, path);
    const notice: Notice = { kind, path, message, seq: this.nextSeq++, actions };
    const idx = this.items.findIndex((n) => mergeKey(n.kind, n.path) === key);
    if (idx >= 0) this.items[idx] = notice;
    else this.items.push(notice);
    this.render();
  }

  /** 種別の通知を解消で取り除く（設定エラー解消等・要件11.1 自動消滅条件）。 */
  dismiss(kind: NoticeKind, path: string | null): void {
    const key = mergeKey(kind, path);
    this.items = this.items.filter((n) => mergeKey(n.kind, n.path) !== key);
    this.render();
  }

  /**
   * 使い捨てトーストを積み、その **固有 seq** を返す（簡易 notify 用・eval medium #25）。
   * 採番した seq だけを dismissToast で消すので、連続する同レベルのトーストが互いを早期消去しない。
   * 上限超過時は最古を捨てる（自動消滅もあるので通常は溜まらない）。
   */
  pushToast(message: string, level: ToastLevel): number {
    const seq = this.toastSeq++;
    this.toasts.push({ level, message, seq });
    if (this.toasts.length > NoticeQueue.MAX_TOASTS) {
      this.toasts.splice(0, this.toasts.length - NoticeQueue.MAX_TOASTS);
    }
    this.render();
    return seq;
  }

  /** 指定 seq のトーストだけを消す（自動消滅/手動クローズ共通・eval medium #25）。 */
  dismissToast(seq: number): void {
    const before = this.toasts.length;
    this.toasts = this.toasts.filter((t) => t.seq !== seq);
    if (this.toasts.length !== before) this.render();
  }

  /** いま表示する通知を解決する（優先順位→新しさ・最大3本＋他N件）。 */
  private resolve(): { visible: Notice[]; overflow: number } {
    const shown = this.items.filter((n) => n.path === null || n.path === this.activeTab);
    shown.sort((a, b) => PRIORITY[a.kind] - PRIORITY[b.kind] || b.seq - a.seq);
    return { visible: shown.slice(0, MAX_VISIBLE), overflow: Math.max(0, shown.length - MAX_VISIBLE) };
  }

  /** DOM へ描画する（role=status/aria-live は host 側・design doc 17章）。 */
  private render(): void {
    const { visible, overflow } = this.resolve();
    const el = host();
    el.replaceChildren();
    for (const n of visible) {
      const item = document.createElement("div");
      item.className = "notice";
      item.dataset.kind = n.kind;
      const text = document.createElement("span");
      text.textContent = n.message;
      item.appendChild(text);
      for (const a of n.actions ?? []) {
        const btn = document.createElement("button");
        btn.type = "button";
        btn.textContent = a.label;
        btn.addEventListener("click", a.onClick);
        item.appendChild(btn);
      }
      // 閉じるボタン（自動消滅しない種別はユーザーが閉じられる・要件11.1）。
      const close = document.createElement("button");
      close.type = "button";
      close.className = "notice-close";
      close.setAttribute("aria-label", "通知を閉じる");
      close.textContent = "×";
      close.addEventListener("click", () => this.dismiss(n.kind, n.path));
      item.appendChild(close);
      el.appendChild(item);
    }
    if (overflow > 0) {
      const more = document.createElement("div");
      more.className = "notice notice-overflow";
      more.textContent = `他${overflow}件`;
      el.appendChild(more);
    }
    // 使い捨てトースト（簡易 notify）は意味付き通知の後ろに、積んだ順（古い→新しい）で出す。
    // data-kind はレベル（info/warn/error）。error は既存 CSS の error 枠（alert/Highlight）が当たる。
    for (const t of this.toasts) {
      const item = document.createElement("div");
      item.className = "notice";
      item.dataset.kind = t.level;
      const text = document.createElement("span");
      text.textContent = t.message;
      item.appendChild(text);
      // 閉じるボタン（自動消滅前でも手動で閉じられる）。その seq だけを消す。
      const close = document.createElement("button");
      close.type = "button";
      close.className = "notice-close";
      close.setAttribute("aria-label", "通知を閉じる");
      close.textContent = "×";
      close.addEventListener("click", () => this.dismissToast(t.seq));
      item.appendChild(close);
      el.appendChild(item);
    }
  }
}

/** アプリ全体で1つの通知キュー。 */
export const notices = new NoticeQueue();

/**
 * 簡易通知（旧 notify(message, level) 呼び出し・eval medium #25）。
 *
 * **使い捨てトースト**として積む。以前はレベルを意味付き種別へ写していた（error→conflict 等）が、
 * これには 2 つの欠陥があった:
 *  1. error→conflict 化け: 本来「閉じるまで残す」種別（conflict は自動消滅しない）へ写るため簡易
 *     error が永続化し、本物の衝突通知と取り違える（5 秒タイマーも発火しなかった）。
 *  2. dismiss(kind, null) が同一種別+global を **全消し** するため、連続する同種の簡易通知で
 *     最新のものが古いタイマーに早期消去される。
 * トースト種別を意味付き通知から独立させ、自動消滅は **採番した seq の 1 件だけ** を消すことで両方を断つ。
 */
export function notify(message: string, level: ToastLevel = "info"): void {
  const seq = notices.pushToast(message, level);
  // 簡易通知はすべて一定時間で自動消滅（要件11.1 種類別の自動消滅・トーストは transient）。
  // その seq の 1 件だけを消すので、後続の同レベル通知を巻き込まない。
  window.setTimeout(() => notices.dismissToast(seq), 5000);
}
