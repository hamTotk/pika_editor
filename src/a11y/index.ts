// アクセシビリティ（ARIA 全Web再構築・F6/Shift+F6 ペイン間フォーカス循環＝要件11.5・design doc 17章）。
//
// 旧版は wx ネイティブコントロールが UIA/MSAA のアクセシブルネームを自動供給した。全Web UI では
// これが消えるため ARIA で再構築する（要件11.5 を満たす唯一の手段）。本モジュールは:
// - F6/Shift+F6 のペイン間フォーカス循環をフロントの自前フォーカスマネージャで実装する
//   （要件11.5「F6/Shift+F6 はフロントの自前フォーカスマネージャで実装する」）。
// - 各ペインへ role/aria を付与する初期化を集約する（ツリー/タブ/通知/ステータスは各 ui/* が付与済み）。
//
// キーボードのみで「開く→プレビュー→差分→確認済み」が完走できることを担保する（要件11.4/11.5）。

/** F6 で巡回するペインの ID（フォーカス順・要件11.5）。 */
const PANE_IDS = ["tree", "tabs", "editor-host", "diff-host", "preview-host"] as const;

/** ペインへフォーカスを移す（hidden や非表示ペインは飛ばす）。 */
function focusPane(id: string): boolean {
  const el = document.getElementById(id);
  if (!el || (el as HTMLElement).hidden || el.getAttribute("aria-hidden") === "true") {
    return false;
  }
  // ペイン内のフォーカス可能要素があればそれへ、なければペイン自身を tabindex=-1 でフォーカス。
  // roving tabindex を持つツリー/タブでは「選択中の行（tabindex=0 or aria-selected=true）」を
  // 最優先で拾う。これを最初に試さないと、先頭行（tabindex=-1）へ飛んで選択位置を失う（eval #48・
  // tree.ts は選択行を tabindex=0 にする）。見つからなければ従来のフォールバックへ。
  const focusable =
    el.querySelector<HTMLElement>(
      '[tabindex="0"], [role="tab"][aria-selected="true"], [role="treeitem"][aria-selected="true"]',
    ) ??
    el.querySelector<HTMLElement>(
      '[tabindex]:not([tabindex="-1"]), button:not([disabled]), [role="treeitem"], [role="tab"]',
    );
  if (focusable) {
    focusable.focus();
  } else {
    el.tabIndex = -1;
    el.focus();
  }
  return true;
}

/** 現在フォーカスのあるペインの index を求める（無ければ -1）。 */
function currentPaneIndex(): number {
  const active = document.activeElement;
  if (!active) return -1;
  for (let i = 0; i < PANE_IDS.length; i++) {
    const pane = document.getElementById(PANE_IDS[i]);
    if (pane && pane.contains(active)) return i;
  }
  return -1;
}

/** 次（forward）/前（backward）の表示中ペインへフォーカスを循環移動する（要件11.5）。 */
function cyclePane(forward: boolean): void {
  const start = currentPaneIndex();
  const n = PANE_IDS.length;
  for (let step = 1; step <= n; step++) {
    const idx = forward
      ? (start + step + n) % n
      : (start - step + n) % n;
    if (focusPane(PANE_IDS[idx])) return;
  }
}

/**
 * F6/Shift+F6 のペイン間フォーカス循環を初期化する（要件11.5・design doc 17章）。
 * capture フェーズで拾い、CM6 等の既定処理より先にペイン移動を確定する。
 */
export function initFocusCycling(): void {
  window.addEventListener(
    "keydown",
    (e) => {
      if (e.key === "F6") {
        e.preventDefault();
        // 自前モーダル（名前入力プロンプト等）の表示中はペイン循環を止める（第2巡 回帰修正）。
        // capture フェーズで登録される本ハンドラは main.ts の modalDepth ガードの外なので、
        // ここで .modal-overlay の存在を見てフォーカスを背景ペインへ逃がさない（フォーカストラップ維持）。
        if (document.querySelector(".modal-overlay")) return;
        cyclePane(!e.shiftKey); // Shift+F6 は逆回り。
      }
    },
    true,
  );
}

/**
 * 通知バー/ステータスの aria 属性を確実化する（index.html で土台付与済み・冪等な再保証）。
 *
 * ライブリージョンの用途分離（eval #47・二重読み上げ回避）:
 * - #notifications … トースト/意味付き通知 **専用** の唯一の status ライブリージョン（主読み上げ経路）。
 * - #sr-live …… 差分件数などステータスの **補助読み上げ専用**（role は持たず aria-live のみ・index.html）。
 *   両者は別文言を流す（同一文言を両方へ流さない）ことで二度読みを断つ。announce() は #sr-live のみ、
 *   notify()/notices は #notifications のみへ書き込む（経路を分離済み）。
 */
export function initLandmarks(): void {
  const notifications = document.getElementById("notifications");
  if (notifications) {
    notifications.setAttribute("role", "status");
    notifications.setAttribute("aria-live", "polite");
  }
  const status = document.getElementById("status");
  if (status) {
    // ステータスは表示専用（要件11.1）。読み上げは aria-label（renderStatus が随時更新）。
    status.setAttribute("aria-live", "off");
  }
}

/**
 * スクリーンリーダー専用ライブ領域へ短いメッセージを announce する（要件11.5・eval medium）。
 * 表示専用ステータス（aria-live=off・pointer-events:none）では読み上げ到達が弱い情報
 * （差分あり件数など）を、視覚的に隠した polite ライブ領域から確実に読ませる。
 * 同一テキストの連続更新でも再読み上げされるよう一旦クリアしてから入れる。
 */
export function announce(message: string): void {
  const region = document.getElementById("sr-live");
  if (!region) return;
  region.textContent = "";
  // 次のフレームで入れて aria-live の変化を確実に検知させる。
  requestAnimationFrame(() => {
    region.textContent = message;
  });
}

/** a11y 全体の初期化（main.ts から1回呼ぶ）。 */
export function initA11y(): void {
  initLandmarks();
  initFocusCycling();
}
