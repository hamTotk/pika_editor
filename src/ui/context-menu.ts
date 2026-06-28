// コンテキストメニュー基盤（S7・旧 main.ts から集約）。menu-pop の見た目を流用した単一ポップアップ。
//
// ツリーの右クリック（新規作成/削除）・タブの右クリック（パスをコピー）・全タブ一覧ドロップダウン
// （キーボード起動）で共有する。多重表示を防ぐため開いているメニューは常に 1 つ（openContext）。
// 配置のクランプ・外側クリック/Esc/blur で閉じる・キーボード操作（↑/↓/Home/End/Enter/Space）は
// 旧実装と挙動不変（純粋リファクタ）。

/** コンテキストメニュー 1 項目（"sep" は区切り線）。 */
export type ContextItem =
  | { label: string; danger?: boolean; checked?: boolean; run: () => void }
  | "sep";

/** 現在開いている単一のコンテキストメニュー要素（多重表示を防ぐ）。 */
let openContext: HTMLElement | null = null;

/** コンテキストメニューを閉じる（外側クリック/Esc/ウィンドウblur/アクション後）。内部利用のみ。 */
function closeContextMenu(): void {
  if (openContext) {
    openContext.remove();
    openContext = null;
  }
  document.removeEventListener("pointerdown", onContextOutside, true);
  document.removeEventListener("keydown", onContextKey, true);
  window.removeEventListener("blur", closeContextMenu);
}

function onContextOutside(e: Event): void {
  if (openContext && !openContext.contains(e.target as Node)) closeContextMenu();
}
function onContextKey(e: KeyboardEvent): void {
  if (e.key === "Escape") {
    e.preventDefault();
    closeContextMenu();
  }
}

/**
 * 指定座標へコンテキストメニューを開く（共通土台・menu-pop の見た目を流用）。
 *
 * `opts.focusFirst=true` のとき、項目をフォーカス可能（tabIndex=0）にし先頭へフォーカスを当て、
 * ↑/↓/Home/End で項目移動・Enter/Space で実行できるようにする（全タブ一覧ドロップダウンなど
 * キーボード起動するメニュー向け・要件11.5）。既定（右クリックメニュー）は従来どおりフォーカスを
 * 動かさず click のみで使う（既存挙動を変えない）。
 */
export function openContextMenu(
  items: ContextItem[],
  x: number,
  y: number,
  opts?: { focusFirst?: boolean },
): void {
  closeContextMenu();
  const focusable = opts?.focusFirst ?? false;
  const menu = document.createElement("div");
  menu.className = "menu-pop context-menu";
  menu.setAttribute("role", "menu");
  for (const item of items) {
    if (item === "sep") {
      const sep = document.createElement("div");
      sep.className = "msep";
      sep.setAttribute("role", "separator");
      menu.appendChild(sep);
      continue;
    }
    const row = document.createElement("div");
    row.className = item.danger ? "mrow danger" : "mrow";
    row.setAttribute("role", "menuitem");
    // キーボード起動メニューは矢印移動の対象にするためフォーカス可能にする。右クリックは従来どおり -1。
    row.tabIndex = focusable ? 0 : -1;
    if (item.checked) {
      row.classList.add("checked");
      row.setAttribute("aria-checked", "true");
    }
    const label = document.createElement("span");
    label.className = "mlabel";
    label.textContent = item.label;
    row.appendChild(label);
    row.addEventListener("click", () => {
      closeContextMenu();
      item.run();
    });
    // 矢印/Home/End/Enter のキーボード操作（フォーカスされた項目でのみ発火＝右クリックには影響しない）。
    row.addEventListener("keydown", (e) => onContextRowKey(e, menu));
    menu.appendChild(row);
  }
  // 画面外へはみ出さないよう、仮表示で寸法を測ってから位置をクランプする。
  menu.style.position = "fixed";
  menu.style.visibility = "hidden";
  menu.style.zIndex = "2000";
  document.body.appendChild(menu);
  const rect = menu.getBoundingClientRect();
  const left = Math.max(4, Math.min(x, window.innerWidth - rect.width - 4));
  const top = Math.max(4, Math.min(y, window.innerHeight - rect.height - 4));
  menu.style.left = `${left}px`;
  menu.style.top = `${top}px`;
  menu.style.visibility = "visible";
  openContext = menu;
  // キーボード起動メニューは開いたら先頭項目へフォーカスする（↑/↓ で移動できる起点を作る）。
  if (focusable) {
    menu.querySelector<HTMLElement>(".mrow")?.focus();
  }
  // 外側クリック/Esc/blur で閉じる。トリガとなった contextmenu ジェスチャの後続イベントで
  // 即閉じないよう、登録は次フレームへ遅延する（capture フェーズで先取り）。
  setTimeout(() => {
    document.addEventListener("pointerdown", onContextOutside, true);
    document.addEventListener("keydown", onContextKey, true);
    window.addEventListener("blur", closeContextMenu);
  }, 0);
}

/** コンテキストメニュー項目のキーボード操作（↑/↓/Home/End 移動・Enter/Space 実行）。 */
function onContextRowKey(e: KeyboardEvent, menu: HTMLElement): void {
  const rows = Array.from(menu.querySelectorAll<HTMLElement>(".mrow"));
  const idx = rows.indexOf(e.currentTarget as HTMLElement);
  if (idx < 0) return;
  if (e.key === "ArrowDown") {
    e.preventDefault();
    rows[(idx + 1) % rows.length]?.focus();
  } else if (e.key === "ArrowUp") {
    e.preventDefault();
    rows[(idx - 1 + rows.length) % rows.length]?.focus();
  } else if (e.key === "Home") {
    e.preventDefault();
    rows[0]?.focus();
  } else if (e.key === "End") {
    e.preventDefault();
    rows[rows.length - 1]?.focus();
  } else if (e.key === "Enter" || e.key === " ") {
    e.preventDefault();
    (e.currentTarget as HTMLElement).click();
  }
}
