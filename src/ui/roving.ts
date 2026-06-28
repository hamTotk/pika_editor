// roving tabindex の共通土台（WAI-ARIA・要件11.5）。tree/tabs/menu の重複を集約（S7）。
//
// roving tabindex = コンテナ（tree/tablist/menubar）内で常に **1 要素だけ** が Tab 到達可能（tabIndex=0）で、
// 残りは -1。Tab で 1 度だけコンテナへ入り、内部移動は矢印キーで行うパターン。各ビューの ARIA・矢印移動・
// フォーカスの細部（aria-selected の付け方・wrap/clamp・自動アクティベーション）は呼び出し側に残し、ここでは
// 「項目のうち1つだけ 0・他は -1」「role での項目収集」という非分岐の共通部分だけを提供する（挙動不変）。

/**
 * items のうち active だけ Tab 到達可能（tabIndex=0）にし、他は -1 にする。
 * `active` は要素そのもの、または index で指定する（index が範囲外なら全て -1）。
 */
export function applyRovingTabindex(items: HTMLElement[], active: HTMLElement | number): void {
  items.forEach((it, i) => {
    const on = typeof active === "number" ? i === active : it === active;
    it.tabIndex = on ? 0 : -1;
  });
}

/** 指定 role の子要素を DOM 順で収集する（roving 対象の項目収集）。 */
export function collectByRole(container: HTMLElement, role: string): HTMLElement[] {
  return Array.from(container.querySelectorAll<HTMLElement>(`[role="${role}"]`));
}
