// Tauri command 境界の薄い型付きラッパ。frontend からの invoke を一箇所に集約する。
// design doc 3章: frontend -> backend は invoke、backend -> frontend は emit。
import { invoke } from "@tauri-apps/api/core";

/** ツリー 1 段分のエントリ（src-tauri commands::TreeEntry と対応）。 */
export interface TreeEntry {
  name: string;
  path: string;
  is_dir: boolean;
}

/** フォルダを開いて直下のエントリ一覧を取得する。 */
export function openWorkspace(path: string): Promise<TreeEntry[]> {
  return invoke<TreeEntry[]>("open_workspace", { path });
}

/** ファイル内容を読む。 */
export function readFile(path: string): Promise<string> {
  return invoke<string>("read_file", { path });
}

/** ファイルを保存する。 */
export function saveFile(path: string, content: string): Promise<void> {
  return invoke<void>("save_file", { path, content });
}
