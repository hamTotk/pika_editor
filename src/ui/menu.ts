// カスタムメニューバーのドロップダウン（UIブラッシュアップ T8・差分 A1/A2/A3・ui-mock .menu-pop）。
//
// pika は「全Web描画」原則（CLAUDE.md）のため OS ネイティブメニュー（Tauri menu）は使わず、
// メニューバー（#menubar の .mi）とドロップダウン（.menu-pop）を HTML/CSS/TS で実装する。
//
// 本モジュールの責務:
//   - メニュー定義（MenuSpec）を受け取り、.mi クリック/ホバーで対応 .menu-pop を直下に開く。
//   - 項目(.mrow)クリックでアクション実行＋閉じる。外側クリック・Esc で閉じる。
//   - 開いている間に別 .mi へホバー/クリックで切替（メニューバーのよくある挙動）。
//   - 現在状態は .mrow.checked（✓）で示し、ショートカットは .acc に表記する（shortcuts.ts と一致）。
//   - disabled は **メニューを開いた時点で都度計算**して .mrow に反映する（aria-disabled＋クリック無効）。
//   - a11y: menubar/menuitem/menu の role、aria-haspopup/aria-expanded、←/→（メニューバー）・
//     ↑/↓（ドロップダウン）・Enter 実行・Esc 閉じる、を実装する（要件11.5）。
//     さらに menubar パターンの **roving tabindex**（Tab は menubar へ 1 度だけ入り、内部移動は ←/→）
//     を実装し、ARIA role(menubar/menuitem) の宣言と実挙動を一致させる（eval #31）。
//
// メニュー定義は **開くたびに評価する関数（build）** にして、checked/disabled/サブ表記（acc）が
// その時点の state を反映できるようにする（main.ts は state を参照する build を渡す）。

/** ドロップダウン 1 項目の定義（区切り or 実行項目）。 */
export type MenuItemSpec =
  | { kind: "separator" }
  | {
      kind: "item";
      /** 行のラベル（左）。 */
      label: string;
      /** ショートカット表記（右・.acc）。サブ値表記（改行コード ▸ LF 等）も acc で出す。 */
      accel?: string;
      /** ✓ を付けるか（現在の表示モード/テーマ/折り返し ON など）。 */
      checked?: boolean;
      /** 無効化するか（aria-disabled＋クリック無効）。 */
      disabled?: boolean;
      /** クリック/Enter で実行する操作。disabled のときは呼ばれない。 */
      onSelect?: () => void;
    };

/** 1 メニュー（ファイル/編集/…）の定義。build は開くたびに評価して最新 state を反映する。 */
export interface MenuSpec {
  /** #menubar 上の .mi の data-menu 値（"file"/"edit"/…）。 */
  id: string;
  /** 開くたびに項目配列を組み立てる（checked/disabled を都度算出するため関数にする）。 */
  build: () => MenuItemSpec[];
}

/** メニューバー全体を制御するハンドル。 */
export interface MenuBarHandle {
  /** 開いているメニューを閉じる（外部から強制クローズしたいとき）。 */
  close(): void;
  /** 破棄（リスナ解除）。 */
  destroy(): void;
}

/**
 * メニューバーを初期化し、開閉・キーボード操作・項目実行を結線する。
 *
 * @param menubar  #menubar 要素（.mi を子に持つ・role=menubar）。
 * @param layer    ドロップダウン(.menu-pop)を載せる絶対配置レイヤ（#menu-layer）。
 * @param specs    各メニューの定義（id は .mi の data-menu と一致させる）。
 */
export function initMenuBar(
  menubar: HTMLElement,
  layer: HTMLElement,
  specs: MenuSpec[],
): MenuBarHandle {
  // data-menu -> spec の対応表。
  const specById = new Map<string, MenuSpec>();
  for (const s of specs) specById.set(s.id, s);
  // メニューバー上の .mi（順序は DOM 順＝←/→ 移動の順序）。
  const triggers = Array.from(menubar.querySelectorAll<HTMLButtonElement>(".mi[data-menu]"));

  // 現在開いているメニューの data-menu（閉じているとき null）。
  let openId: string | null = null;

  /**
   * roving tabindex（menubar パターン・要件11.5）。menubar は Tab で 1 度だけ入り、内部移動は ←/→ で
   * 行う。そのため「いま入口になっている 1 個」だけ tabindex=0、他は -1 にする。←/→ で focus を移す際に
   * これも更新し、最後にフォーカスした .mi が次回 Tab の入口になるようにする。
   */
  function setRoving(active: HTMLElement): void {
    for (const t of triggers) t.tabIndex = t === active ? 0 : -1;
  }
  // 初期入口は先頭の .mi（無ければ何もしない）。
  if (triggers.length > 0) setRoving(triggers[0]);

  /** トリガ(.mi)の aria-expanded を実状態へ同期する。 */
  function syncExpanded(): void {
    for (const t of triggers) {
      t.setAttribute("aria-expanded", String(t.dataset.menu === openId));
    }
  }

  /** 開いているメニューを閉じる（レイヤを空にし aria を戻す）。 */
  function close(): void {
    if (openId === null) return;
    openId = null;
    layer.replaceChildren();
    syncExpanded();
  }

  /** 指定メニューの .menu-pop をそのトリガ直下に開く（既に開いていれば張り替える）。 */
  function open(id: string): void {
    const spec = specById.get(id);
    const trigger = triggers.find((t) => t.dataset.menu === id);
    if (!spec || !trigger) return;
    openId = id;
    const pop = buildPop(spec, close);
    layer.replaceChildren(pop);
    // トリガ直下へ絶対配置する。pop の offset parent は #menu-layer（#app 全面）なので、
    // トリガ/レイヤの viewport 矩形の差から #menu-layer 基準の座標を求める（左端をトリガに合わせる）。
    const layerRect = layer.getBoundingClientRect();
    const tRect = trigger.getBoundingClientRect();
    pop.style.position = "absolute";
    pop.style.top = `${tRect.bottom - layerRect.top}px`;
    pop.style.left = `${tRect.left - layerRect.left}px`;
    syncExpanded();
    // 開いたら先頭の有効項目へフォーカスし、↑/↓・Enter・Esc を効かせる。
    focusFirstRow(pop);
  }

  /** トリガのクリック=トグル（開→閉 / 閉→開）。 */
  function onTriggerClick(e: MouseEvent): void {
    const id = (e.currentTarget as HTMLElement).dataset.menu;
    if (!id) return;
    if (openId === id) close();
    else open(id);
  }

  /** メニューが開いている間、別 .mi へホバーするとそのメニューへ切替える（OS メニュー風）。 */
  function onTriggerHover(e: MouseEvent): void {
    if (openId === null) return;
    const id = (e.currentTarget as HTMLElement).dataset.menu;
    if (id && id !== openId) open(id);
  }

  /** メニューバー上のキーボード操作（←/→ で .mi 間移動・↓/Enter で開く・Esc で閉じる）。 */
  function onTriggerKey(e: KeyboardEvent): void {
    const idx = triggers.indexOf(e.currentTarget as HTMLButtonElement);
    if (idx < 0) return;
    if (e.key === "ArrowRight") {
      e.preventDefault();
      const next = triggers[(idx + 1) % triggers.length];
      setRoving(next); // 次回 Tab の入口を移す（roving tabindex・要件11.5）。
      next.focus();
      if (openId !== null) open(next.dataset.menu ?? "");
    } else if (e.key === "ArrowLeft") {
      e.preventDefault();
      const prev = triggers[(idx - 1 + triggers.length) % triggers.length];
      setRoving(prev);
      prev.focus();
      if (openId !== null) open(prev.dataset.menu ?? "");
    } else if (e.key === "ArrowDown" || e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      open((e.currentTarget as HTMLElement).dataset.menu ?? "");
    } else if (e.key === "Escape") {
      close();
    }
  }

  // クリック等でトリガにフォーカスが入ったら、それを roving の入口にする（次回 Tab 復帰位置の整合）。
  function onTriggerFocus(e: FocusEvent): void {
    setRoving(e.currentTarget as HTMLElement);
  }

  for (const t of triggers) {
    t.addEventListener("click", onTriggerClick);
    t.addEventListener("mouseenter", onTriggerHover);
    t.addEventListener("keydown", onTriggerKey);
    t.addEventListener("focus", onTriggerFocus);
  }

  // 外側クリックで閉じる（メニューバー/ドロップダウン外をクリックしたとき）。
  function onDocPointerDown(e: PointerEvent): void {
    if (openId === null) return;
    const target = e.target as Node;
    if (menubar.contains(target) || layer.contains(target)) return;
    close();
  }
  document.addEventListener("pointerdown", onDocPointerDown);

  // Esc はドロップダウン内フォーカス時にも効くよう document でも拾う（トリガへ戻す）。
  function onDocKey(e: KeyboardEvent): void {
    if (openId === null) return;
    if (e.key === "Escape") {
      const id = openId;
      close();
      // フォーカスをトリガへ戻す（キーボード操作の位置を見失わない）。
      triggers.find((t) => t.dataset.menu === id)?.focus();
    }
  }
  document.addEventListener("keydown", onDocKey);

  syncExpanded();

  return {
    close,
    destroy() {
      close();
      document.removeEventListener("pointerdown", onDocPointerDown);
      document.removeEventListener("keydown", onDocKey);
      for (const t of triggers) {
        t.removeEventListener("click", onTriggerClick);
        t.removeEventListener("mouseenter", onTriggerHover);
        t.removeEventListener("keydown", onTriggerKey);
        t.removeEventListener("focus", onTriggerFocus);
      }
    },
  };
}

/** spec を評価して .menu-pop（role=menu）の DOM を組み立てる。 */
function buildPop(spec: MenuSpec, close: () => void): HTMLElement {
  const pop = document.createElement("div");
  pop.className = "menu-pop";
  pop.setAttribute("role", "menu");
  pop.dataset.menu = spec.id;
  // build は「開いた時点」で評価する（checked/disabled が最新 state を反映する）。
  for (const item of spec.build()) {
    if (item.kind === "separator") {
      const sep = document.createElement("div");
      sep.className = "msep";
      sep.setAttribute("role", "separator");
      pop.appendChild(sep);
      continue;
    }
    const row = document.createElement("div");
    row.className = "mrow";
    row.setAttribute("role", "menuitem");
    if (item.checked) row.classList.add("checked");
    const disabled = item.disabled ?? false;
    if (disabled) {
      row.setAttribute("aria-disabled", "true");
      row.classList.add("disabled");
    }
    // 矢印移動の対象にするためフォーカス可能にする（有効項目のみ）。
    row.tabIndex = disabled ? -1 : 0;
    if (item.checked) row.setAttribute("aria-checked", "true");

    const label = document.createElement("span");
    label.className = "mlabel";
    label.textContent = item.label;
    row.appendChild(label);
    if (item.accel) {
      const acc = document.createElement("span");
      acc.className = "acc";
      acc.textContent = item.accel;
      row.appendChild(acc);
    }

    const run = (): void => {
      if (disabled) return;
      close();
      item.onSelect?.();
    };
    row.addEventListener("click", run);
    row.addEventListener("keydown", (e) => onRowKey(e, pop, run));
    pop.appendChild(row);
  }
  return pop;
}

/** ドロップダウン内のキーボード操作（↑/↓ で項目移動・Enter/Space 実行・Esc は document が拾う）。 */
function onRowKey(e: KeyboardEvent, pop: HTMLElement, run: () => void): void {
  const rows = focusableRows(pop);
  const idx = rows.indexOf(e.currentTarget as HTMLElement);
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
    run();
  }
}

/** ドロップダウン内のフォーカス可能な .mrow（disabled 以外）を取る。 */
function focusableRows(pop: HTMLElement): HTMLElement[] {
  return Array.from(pop.querySelectorAll<HTMLElement>('.mrow:not([aria-disabled="true"])'));
}

/** 開いた直後に先頭の有効項目へフォーカスする。 */
function focusFirstRow(pop: HTMLElement): void {
  focusableRows(pop)[0]?.focus();
}
