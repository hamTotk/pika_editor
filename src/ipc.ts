// Tauri command 境界の薄い型付きラッパ。frontend からの invoke を一箇所に集約する。
// design doc 3章: frontend -> backend は invoke、backend -> frontend は emit。
import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";

/** ツリー 1 段分のエントリ（src-tauri commands::TreeEntry と対応）。 */
export interface TreeEntry {
  name: string;
  path: string;
  is_dir: boolean;
}

/** 合成済み外部変更 1 件（src-tauri watcher::FsChangeDto と対応・要件4.2/7.1）。 */
export interface FsChange {
  /** "created" | "modified" | "removed" | "renamed" */
  kind: "created" | "modified" | "removed" | "renamed";
  /** 対象パス（rename では新パス）。 */
  path: string;
  /** rename の旧パス（rename 以外は未設定）。 */
  from?: string;
}

/** `emit('fs-changed')` のペイロード（src-tauri watcher::FsChangedPayload と対応）。 */
export interface FsChangedPayload {
  changes: FsChange[];
}

/** フォルダを開いて直下のエントリ一覧を取得する。 */
export function openWorkspace(path: string): Promise<TreeEntry[]> {
  return invoke<TreeEntry[]>("open_workspace", { path });
}

/** ファイル内容を読む。 */
export function readFile(path: string): Promise<string> {
  return invoke<string>("read_file", { path });
}

/** ファイルを保存する。保存後ハッシュで自己保存を抑制する（backend 側・要件7.1）。 */
export function saveFile(path: string, content: string): Promise<void> {
  return invoke<void>("save_file", { path, content });
}

/** F5（要件7.1/11.2）= オンデマンドの全体再スキャン＋再同期。検知件数を返す。 */
export function f5Resync(): Promise<number> {
  return invoke<number>("f5_resync");
}

/** 外部変更（合成結果）の購読。返り値の関数で購読解除する。 */
export function onFsChanged(
  handler: (payload: FsChangedPayload) => void,
): Promise<UnlistenFn> {
  return listen<FsChangedPayload>("fs-changed", (e) => handler(e.payload));
}

/** 監視モード変更（ポーリング縮退・再同期中など）の通知購読。 */
export function onWatchMode(handler: (message: string) => void): Promise<UnlistenFn> {
  return listen<string>("watch-mode", (e) => handler(e.payload));
}
