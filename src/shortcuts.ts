// フロントのキーディスパッチ表（要件11.2・design doc 19章 主要ショートカット表）。
//
// **正本は pika-core::shortcuts（crates/pika-core/src/shortcuts.rs・cargo test 済みの決定論ゲート）**。
// 本 TS モジュールはその resolve ロジックを 1:1 で写し、フロントの keydown ハンドラが
// 「いまのフォーカスでこのキーは何の操作か」を決定論で解くために使う配線。Rust 側の resolve を
// 変更したら本ファイルも同じ表へ追従させる（パリティ契約）。フロントは結果（Action）に従って
// 既存の onSave/onToggleDiff 等へ振り分けるだけで、判定ロジックを散らさない。
//
// 要件11.2 の核:
// - Ctrl+Enter はフォーカスが差分/プレビューのときだけ「確認済み」を発火（エディタ編集中の誤爆防止）。
//   エディタフォーカス時の確認済みは Ctrl+Shift+Enter。すべて確認済みは Ctrl+Alt+Enter。
// - F8/Shift+F8（次/前の変更）と Ctrl+\ には代替割当（Alt+Down/Up・Ctrl+Shift+E）を併記。

/** 修飾キーの状態（KeyboardEvent から組み立てる・pika-core::shortcuts::Mods と対応）。 */
export interface Mods {
  ctrl: boolean;
  shift: boolean;
  alt: boolean;
}

/** いまフォーカスのあるペイン（Ctrl+Enter 誤爆防止に使う・pika-core::shortcuts::Focus と対応）。 */
export type Focus = "editor" | "diff" | "preview" | "tree" | "other";

/** 発火する操作（pika-core::shortcuts::Action と対応・要件11.2）。 */
export type Action =
  | "open-file" // Ctrl+O
  | "open-folder" // Ctrl+Shift+O
  | "toggle-preview" // Ctrl+E
  | "toggle-split" // Ctrl+\（代替 Ctrl+Shift+E）
  | "toggle-diff" // Ctrl+Shift+D
  | "confirm-file" // Ctrl+Enter（差分/プレビュー時）/ Ctrl+Shift+Enter
  | "confirm-all" // Ctrl+Alt+Enter
  | "next-change" // F8（代替 Alt+Down）
  | "prev-change" // Shift+F8（代替 Alt+Up）
  | "find" // Ctrl+F
  | "replace" // Ctrl+H
  | "save" // Ctrl+S
  | "close-tab" // Ctrl+W
  | "resync"; // F5

/**
 * キー（修飾＋フォーカス）から発火する操作を解決する（pika-core::shortcuts::resolve の写し・要件11.2）。
 *
 * @param key   KeyboardEvent.key 相当。英字は**小文字**で渡す規約（"o"/"e"/"\\"/"Enter"/"F8" 等）。
 * @param mods  修飾キーの状態。
 * @param focus 現在フォーカスのあるペイン（Ctrl+Enter 誤爆防止に使う）。
 * @returns 一致が無ければ null（フロントは既定処理＝CM6 等へ委ねる）。
 */
export function resolveShortcut(key: string, mods: Mods, focus: Focus): Action | null {
  const plainCtrl = mods.ctrl && !mods.shift && !mods.alt;
  const ctrlShift = mods.ctrl && mods.shift && !mods.alt;
  const ctrlAlt = mods.ctrl && !mods.shift && mods.alt;
  const plainAlt = !mods.ctrl && mods.alt && !mods.shift;
  const noMods = !mods.ctrl && !mods.shift && !mods.alt;
  const shiftOnly = !mods.ctrl && mods.shift && !mods.alt;

  if (key === "o" && plainCtrl) return "open-file";
  if (key === "o" && ctrlShift) return "open-folder";
  if (key === "e" && plainCtrl) return "toggle-preview";
  if (key === "e" && ctrlShift) return "toggle-split"; // 分割表示の代替（要件11.2）。
  if (key === "\\" && plainCtrl) return "toggle-split";
  if (key === "d" && ctrlShift) return "toggle-diff";
  if (key === "f" && plainCtrl) return "find";
  if (key === "h" && plainCtrl) return "replace";
  if (key === "s" && plainCtrl) return "save";
  if (key === "w" && plainCtrl) return "close-tab";

  // Enter 系（確認済み）の誤爆防止（要件11.2）。
  if (key === "Enter") {
    if (ctrlAlt) return "confirm-all";
    if (ctrlShift) return "confirm-file";
    if (plainCtrl) {
      // 差分/プレビューにフォーカスがあるときだけ発火（エディタ編集中の誤爆防止）。
      return focus === "diff" || focus === "preview" ? "confirm-file" : null;
    }
    return null;
  }

  // F8 系（次/前の変更）と代替割当（要件11.2）。
  if (key === "F8") {
    if (shiftOnly) return "prev-change";
    if (noMods) return "next-change";
  }
  if (key === "F5" && noMods) return "resync";
  // 代替: Alt+Down / Alt+Up（F8系の JIS/IME 衝突回避＝要件11.2）。
  if (plainAlt) {
    if (key === "ArrowDown") return "next-change";
    if (key === "ArrowUp") return "prev-change";
  }
  return null;
}

/** KeyboardEvent から Mods を取り出す（英字キーは小文字化して resolve へ渡す前段）。 */
export function modsOf(e: KeyboardEvent): Mods {
  return { ctrl: e.ctrlKey || e.metaKey, shift: e.shiftKey, alt: e.altKey };
}

/** KeyboardEvent.key を resolve 規約（英字は小文字）に正規化する。 */
export function normalizeKey(e: KeyboardEvent): string {
  // 1 文字の英字は小文字化（resolve は "o"/"e" 等の小文字を期待）。それ以外（"Enter"/"F8"/"\\"）はそのまま。
  return e.key.length === 1 ? e.key.toLowerCase() : e.key;
}
