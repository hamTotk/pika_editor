// テーマ適用（design doc 10章・ui-design 2章）。
// 配色トークンの単一源は styles/tokens.css。ここは html[data-theme] の切替のみを担う。
// OS テーマ変更の Tauri テーマイベント受信・state.json 永続は sprint 7 で配線する。

export type ThemeMode = "light" | "dark" | "system";

/** html 要素の data-theme を切替える（既定: system 追従）。 */
export function applyTheme(mode: ThemeMode): void {
  document.documentElement.setAttribute("data-theme", mode);
}

/**
 * 現在のテーマモード（html[data-theme]）を返す（表示メニューの ✓ 表示に使う・UIブラッシュアップ T8）。
 * 未設定/不正値は system 扱い。
 */
export function currentTheme(): ThemeMode {
  const v = document.documentElement.getAttribute("data-theme");
  return v === "light" || v === "dark" ? v : "system";
}

/** 起動時の初期テーマを適用する。state.json 連携前は system 固定。 */
export function initTheme(): void {
  applyTheme("system");
}
