// テーマ適用（design doc 10章・ui-design 2章）。
// 配色トークンの単一源は styles/tokens.css。ここは html[data-theme] の切替のみを担う。
// OS テーマ変更の Tauri テーマイベント受信・state.json 永続は sprint 7 で配線する。

export type ThemeMode = "light" | "dark" | "system";

/**
 * 任意入力を有効な ThemeMode へ正規化する（不正は "system"）。
 * state.json 由来の破損値で未知文字列が data-theme に入り OS 追従が壊れるのを防ぐ
 * （currentTheme() の正規化と揃える・eval #50）。
 */
function normalizeMode(mode: unknown): ThemeMode {
  return mode === "light" || mode === "dark" || mode === "system" ? mode : "system";
}

/** html 要素の data-theme を切替える（既定: system 追従）。不正値は "system" に倒す。 */
export function applyTheme(mode: ThemeMode): void {
  document.documentElement.setAttribute("data-theme", normalizeMode(mode));
}

/**
 * 現在のテーマモード（html[data-theme]）を返す（表示メニューの ✓ 表示に使う・UIブラッシュアップ T8）。
 * 未設定/不正値は system 扱い。
 */
export function currentTheme(): ThemeMode {
  return normalizeMode(document.documentElement.getAttribute("data-theme"));
}

/** 起動時の初期テーマを適用する。state.json 連携前は system 固定。 */
export function initTheme(): void {
  applyTheme("system");
}
