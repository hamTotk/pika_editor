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

/** 後方互換の簡易レベル（旧 notify(message, kind) 互換用）。 */
export type NoticeLevel = "info" | "warn" | "error";

/** 種別の表示優先順位（小さいほど先・要件11.1）。 */
const PRIORITY: Record<NoticeKind, number> = {
  conflict: 0,
  "settings-error": 1,
  "external-resource": 2,
  "script-detected": 3,
  "huge-file-limit": 4,
  "file-removed": 5,
};

/** 種別が解消時に自動消滅するか（要件11.1）。衝突だけは閉じるまで残す。 */
const AUTO_DISMISS: Record<NoticeKind, boolean> = {
  conflict: false,
  "settings-error": true,
  "external-resource": true,
  "script-detected": true,
  "huge-file-limit": true,
  "file-removed": true,
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

const host = () => document.getElementById("notifications") as HTMLElement;

/** 通知キュー（決定論モデル・pika-core::notify_queue と同規則）。 */
class NoticeQueue {
  private items: Notice[] = [];
  private nextSeq = 0;
  private activeTab: string | null = null;

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

  /** タブを閉じたとき、そのタブ固有通知を全消去する。 */
  dismissTab(path: string): void {
    this.items = this.items.filter((n) => n.path !== path);
    this.render();
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
  }
}

/** アプリ全体で1つの通知キュー。 */
export const notices = new NoticeQueue();

/**
 * 後方互換の簡易通知（旧 notify(message, kind) 呼び出し）。
 * レベルを種別へ写してグローバル通知として積む（既存呼び出しを壊さない）。
 */
export function notify(message: string, level: NoticeLevel = "info"): void {
  const kind: NoticeKind =
    level === "error" ? "conflict" : level === "warn" ? "settings-error" : "file-removed";
  notices.push(kind, null, message);
  // 情報通知は一定時間で自動消滅（要件11.1 種類別の自動消滅）。
  if (AUTO_DISMISS[kind]) {
    window.setTimeout(() => notices.dismiss(kind, null), 5000);
  }
}
