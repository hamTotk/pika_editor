// ビュー別の5状態（Ideal/Empty/Loading/Partial/Error＝ui-design 15章）。
// 状態解決の決定論部分は pika-core::view_state と同規則。本モジュールは DOM 提示（CTA・進捗・
// 縮退理由・次の一手）を担う（描画は系統C で検証）。

/** Empty の3分岐（ui-design 15章「3分岐で文言を変える」＝要件10章）。 */
export type EmptyReason = "no-folder" | "search-no-results" | "all-consumed";

const EMPTY_MESSAGE: Record<EmptyReason, string> = {
  "no-folder": "フォルダを開いてください",
  "search-no-results": "条件に一致するものがありませんでした",
  "all-consumed": "未読はありません",
};

/** 縮退理由（Partial・要件2.2「黙って切らず理由＋手動再有効化」）。 */
export type DegradeReason =
  | "preview-off"
  | "diff-off"
  | "highlight-off"
  | "wrap-off"
  | "read-only-viewer"
  | "baseline-pending";

const DEGRADE_MESSAGE: Record<DegradeReason, string> = {
  "preview-off": "大きいファイルのためプレビューを自動的に無効にしました",
  "diff-off": "大きいファイルのため差分を自動的に無効にしました",
  "highlight-off": "ハイライトを自動的に無効にしました",
  "wrap-off": "長い行のため折り返しを自動的に無効にしました",
  "read-only-viewer": "非常に大きいファイルのため読み取り専用で表示しています",
  "baseline-pending": "差分基準を取得中です",
};

/** バックエンドの DegradeFlagsDto（document.rs）から Partial の縮退理由列を組み立てる（要件2.2）。 */
export function degradeReasonsFromFlags(flags: {
  preview_off: boolean;
  diff_off: boolean;
  highlight_off: boolean;
  wrap_off: boolean;
  editing_off: boolean;
}): DegradeReason[] {
  const out: DegradeReason[] = [];
  if (flags.preview_off) out.push("preview-off");
  if (flags.diff_off) out.push("diff-off");
  if (flags.highlight_off) out.push("highlight-off");
  if (flags.wrap_off) out.push("wrap-off");
  if (flags.editing_off) out.push("read-only-viewer");
  return out;
}

/** Empty 状態の文言を引く（3分岐・ui-design 15章）。 */
export function emptyMessage(reason: EmptyReason): string {
  return EMPTY_MESSAGE[reason];
}

/** 縮退理由の文言を引く（要件2.2）。 */
export function degradeMessage(reason: DegradeReason): string {
  return DEGRADE_MESSAGE[reason];
}
