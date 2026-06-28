// メニュー定義（5メニュー＝ファイル/編集/表示/移動/ヘルプ）の組み立て（S8・main.ts から抽出）。
//
// buildMenuSpecs は **開くたびに評価される** build 関数の集合を返す。各 build は checked/disabled/
// 現在値（エンコーディング/改行/テーマ/折り返し）をその時点の state から算出し、各項目は注入された
// ハンドラ（ctx）へ結線する。メニュー構成・有効無効条件・各アクションの呼び出し先は main.ts 在籍時から
// 一切変えていない（純粋リファクタ）。state とハンドラは ctx（参照＋コールバック）で注入する。

import type { MenuSpec, MenuItemSpec } from "./menu";
import type { AppShellState, OpenTab } from "../app/types";
import type { DocEncoding, LineEnding } from "../ipc";
import type { ViewMode } from "../preview";
import type { Action } from "../shortcuts";
import { currentTheme, type ThemeMode } from "../theme";

/** buildMenuSpecs の依存（state 参照＋main.ts のハンドラ群）。挙動不変のため main 在籍時と同じ呼び出し先へ繋ぐ。 */
export interface MenuSpecsContext {
  /** メインWebView シェルの可変状態（読み取り専用に使う。build が live に評価する）。 */
  state: AppShellState;
  onOpenFolder(): void;
  onOpenFile(): void;
  onSave(): void;
  onSaveAs(): void;
  onOpenLogFolder(): void;
  onConfirmAll(): void;
  onRollback(): void;
  onToggleDiff(): void;
  onToggleWrap(): void;
  onSetTheme(mode: ThemeMode): void;
  onF5(): void;
  onShowVersion(): void;
  setViewMode(mode: ViewMode): void;
  reopenActiveWithEncoding(enc: DocEncoding): void;
  dispatchAction(action: Action): void;
  /** アクティブが編集可能なテキストタブか（「名前を付けて保存」のゲート・ペイン可視は問わない）。 */
  canEditActiveText(): boolean;
  /** エディタが可視かつ編集可能か（検索/置換/太字などカーソル前提の編集系のゲート）。 */
  canSearch(): boolean;
}

/** アクティブタブのエンコーディング表記（表示メニューの現在値表示用・表示専用）。 */
export function encodingLabel(enc: DocEncoding, hasBom: boolean): string {
  const base =
    enc === "utf-8"
      ? "UTF-8"
      : enc === "utf-16le"
        ? "UTF-16 LE"
        : enc === "utf-16be"
          ? "UTF-16 BE"
          : "Shift_JIS";
  return hasBom ? `${base} (BOM)` : base;
}

/** アクティブタブの改行コード表記（表示メニューの現在値表示用・表示専用）。 */
export function lineEndingLabel(le: LineEnding): string {
  switch (le) {
    case "lf":
      return "LF";
    case "crlf":
      return "CRLF";
    case "cr":
      return "CR";
    case "mixed":
      return "混在";
    default:
      return "—";
  }
}

/**
 * 5つのメニュー（ファイル/編集/表示/移動/ヘルプ）の定義を組み立てる（UIブラッシュアップ T8）。
 * build は**開くたびに**評価され checked/disabled/現在値（エンコーディング/改行/テーマ/折り返し）を反映する。
 * 各項目は注入されたハンドラ（ctx）へ結線する。backend 未実装（改行変換・OS で開く）は
 * 項目を出さない or 案内に留める（要件14章「足さない」を厳守）。
 */
export function buildMenuSpecs(ctx: MenuSpecsContext): MenuSpec[] {
  const state = ctx.state;
  const hasActive = (): boolean => !!state.active;
  const activeTab = (): OpenTab | undefined =>
    state.tabs.find((t) => t.path === state.active);
  // 「すべて確認済み」に対象があるか（無ければ無効化せず実行時に空案内する＝既存挙動を維持）。
  return [
    {
      id: "file",
      build: (): MenuItemSpec[] => [
        { kind: "item", label: "フォルダを開く…", accel: "Ctrl+Shift+O", onSelect: () => ctx.onOpenFolder() },
        { kind: "item", label: "ファイルを開く…", accel: "Ctrl+O", onSelect: () => ctx.onOpenFile() },
        { kind: "separator" },
        {
          kind: "item",
          label: "保存",
          accel: "Ctrl+S",
          disabled: !hasActive() || state.busy,
          onSelect: () => ctx.onSave(),
        },
        {
          kind: "item",
          label: "名前を付けて保存…",
          // 編集可能テキストタブなら**ペイン可視を問わず**対象（プレビューのみでもプレーン保存同様に効く・
          // Codex P2 是正）。現在のエディタ内容を別パスへ書き出すだけで可視は不要なため canEditActiveText で判定。
          disabled: !ctx.canEditActiveText() || state.busy,
          onSelect: () => ctx.onSaveAs(),
        },
        { kind: "separator" },
        { kind: "item", label: "ログフォルダを開く", onSelect: () => ctx.onOpenLogFolder() },
      ],
    },
    {
      id: "edit",
      build: (): MenuItemSpec[] => [
        {
          kind: "item",
          label: "すべて確認済み",
          accel: "Ctrl+Alt+Enter",
          disabled: state.busy,
          onSelect: () => ctx.onConfirmAll(),
        },
        {
          kind: "item",
          label: "確認済み時点に戻す",
          disabled: !hasActive() || state.busy,
          onSelect: () => ctx.onRollback(),
        },
        { kind: "separator" },
        {
          kind: "item",
          label: "検索",
          accel: "Ctrl+F",
          disabled: !hasActive(),
          onSelect: () => ctx.dispatchAction("find"),
        },
        {
          kind: "item",
          label: "置換",
          accel: "Ctrl+H",
          disabled: !hasActive(),
          onSelect: () => ctx.dispatchAction("replace"),
        },
        { kind: "separator" },
        // 編集補助コマンド（要件5.1）。キーバインドが主経路で、メニューは発見性のための薄い再掲。
        // canSearch()＝エディタが可視かつ編集可能（テキスト/非削除/非editingOff）と同条件で出す。
        {
          kind: "item",
          label: "太字",
          accel: "Ctrl+B",
          disabled: !ctx.canSearch(),
          onSelect: () => state.editor?.toggleBold(),
        },
        {
          kind: "item",
          label: "斜体",
          accel: "Ctrl+I",
          disabled: !ctx.canSearch(),
          onSelect: () => state.editor?.toggleItalic(),
        },
        {
          kind: "item",
          label: "チェックボックス",
          accel: "Ctrl+L",
          disabled: !ctx.canSearch(),
          onSelect: () => state.editor?.toggleCheckbox(),
        },
      ],
    },
    {
      id: "view",
      build: (): MenuItemSpec[] => {
        const enabled = hasActive();
        const tab = activeTab();
        const rows: MenuItemSpec[] = [
          {
            kind: "item",
            label: "ソース",
            disabled: !enabled,
            checked: state.viewMode === "source",
            onSelect: () => ctx.setViewMode("source"),
          },
          {
            kind: "item",
            label: "分割",
            accel: "Ctrl+\\",
            disabled: !enabled,
            checked: state.viewMode === "split",
            onSelect: () => ctx.setViewMode("split"),
          },
          {
            kind: "item",
            label: "プレビュー",
            accel: "Ctrl+E",
            disabled: !enabled,
            checked: state.viewMode === "preview",
            onSelect: () => ctx.setViewMode("preview"),
          },
          {
            kind: "item",
            label: "差分",
            accel: "Ctrl+Shift+D",
            disabled: !enabled,
            checked: state.diffOn,
            onSelect: () => ctx.onToggleDiff(),
          },
          { kind: "separator" },
          {
            kind: "item",
            label: "折り返し",
            disabled: !enabled,
            checked: state.lineWrapping,
            onSelect: () => ctx.onToggleWrap(),
          },
          { kind: "separator" },
          // エンコーディングは「指定して開き直す」を実結線（要件5.6 Reopen）。改行コードは現在値の表示のみ
          // （変換 backend は未実装＝要件14章）。アクティブタブが無いときは出さない。
          ...(tab
            ? ([
                // エンコーディングを指定して開き直す（要件5.6 Reopen）。見出しに現在値を表示し、
                // 下の4項目で強制再デコードする。入れ子サブメニュー非対応のためフラット展開（テーマと同型）。
                {
                  kind: "item",
                  label: "エンコーディングを指定して開き直す",
                  accel: encodingLabel(tab.encoding, tab.hasBom),
                  disabled: true,
                },
                ...(["utf-8", "utf-16le", "utf-16be", "shift_jis"] as DocEncoding[]).map(
                  (e): MenuItemSpec => ({
                    kind: "item",
                    label: `　${encodingLabel(e, false)}`,
                    checked: tab.encoding === e,
                    // 削除済みタブは実体が無く開き直せない（backend verify_read も弾くが UI でも無効化）。
                    // 巨大ファイル（編集不可＝第2段階以降）も無効化（backend reopen 拒否と同条件）。
                    disabled: tab.deleted || tab.editingOff,
                    onSelect: () => ctx.reopenActiveWithEncoding(e),
                  }),
                ),
                { kind: "separator" },
                {
                  kind: "item",
                  label: "改行コード",
                  accel: lineEndingLabel(tab.lineEnding),
                  disabled: true,
                },
                { kind: "separator" },
              ] as MenuItemSpec[])
            : []),
          {
            kind: "item",
            label: "テーマ: ライト",
            checked: currentTheme() === "light",
            onSelect: () => ctx.onSetTheme("light"),
          },
          {
            kind: "item",
            label: "テーマ: ダーク",
            checked: currentTheme() === "dark",
            onSelect: () => ctx.onSetTheme("dark"),
          },
          {
            kind: "item",
            label: "テーマ: システム",
            checked: currentTheme() === "system",
            onSelect: () => ctx.onSetTheme("system"),
          },
        ];
        return rows;
      },
    },
    {
      id: "go",
      build: (): MenuItemSpec[] => [
        {
          kind: "item",
          label: "次の変更",
          accel: "F8",
          disabled: !state.diff,
          onSelect: () => ctx.dispatchAction("next-change"),
        },
        {
          kind: "item",
          label: "前の変更",
          accel: "Shift+F8",
          disabled: !state.diff,
          onSelect: () => ctx.dispatchAction("prev-change"),
        },
        { kind: "separator" },
        { kind: "item", label: "再同期", accel: "F5", onSelect: () => ctx.onF5() },
      ],
    },
    {
      id: "help",
      build: (): MenuItemSpec[] => [
        { kind: "item", label: "バージョン情報", onSelect: () => ctx.onShowVersion() },
      ],
    },
  ];
}
