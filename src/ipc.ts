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

/** 行内差分セグメント（src-tauri snapshot::SegmentDto と対応・要件8.2）。 */
export interface DiffSegment {
  /** 変わった部分か（色だけに依存せず下線/太字で表す＝要件8.2/11.5）。 */
  changed: boolean;
  /** セグメント本文。 */
  text: string;
}

/** unified 差分の 1 行（src-tauri snapshot::DiffLineDto と対応・要件8.2）。 */
export interface DiffLine {
  /** "equal" | "insert" | "delete" */
  tag: "equal" | "insert" | "delete";
  /** 旧側行番号（追加行は未設定）。 */
  old_line_no?: number;
  /** 新側行番号（削除行は未設定）。 */
  new_line_no?: number;
  /** 行本文（改行なし）。 */
  content: string;
  /** 行内差分セグメント（置換行のみ。共通部分は changed=false）。 */
  segments: DiffSegment[];
}

/** ファイル差分（src-tauri snapshot::FileDiffDto と対応・要件8.2）。 */
export interface FileDiff {
  lines: DiffLine[];
  change_count: number;
  /** 内容ベースラインを持つか（false なら差分・巻き戻し非対象＝ハッシュのみ）。 */
  has_baseline_content: boolean;
}

/** ベースライン vs 現在内容の差分を計算する（要件8.2）。current=編集バッファ or ディスク内容。 */
export function computeFileDiff(path: string, current: string): Promise<FileDiff> {
  return invoke<FileDiff>("compute_file_diff", { path, current });
}

/**
 * 「確認済みにする」（要件8.3）。確定直前にディスク再照合し、変化していなければベースライン更新。
 * 返り値 true=確認済み確定 / false=ディスクが変化していたため中断（再差分を促す）。
 */
export function confirmFile(path: string): Promise<boolean> {
  return invoke<boolean>("confirm_file", { path });
}

/** 「すべて確認済みにする」の結果（src-tauri snapshot::ConfirmAllResult と対応・要件8.3）。 */
export interface ConfirmAllResult {
  /** 確認済みにした件数。 */
  updated: number;
  /** 処理中に変化したためスキップした件数（未読のまま残る）。 */
  skipped: number;
  /** baseline-replace バッチへ一括退避した件数（一括取り消し対象）。 */
  stashed: number;
}

/**
 * 「すべて確認済みにする」（要件8.3）。paths=実行開始時点でフリーズした未読集合。
 * 更新前ベースラインは baseline-replace バッチへ一括退避される。
 */
export function confirmAll(paths: string[]): Promise<ConfirmAllResult> {
  return invoke<ConfirmAllResult>("confirm_all", { paths });
}

/**
 * 「確認済み時点に戻す」（ファイル単位巻き戻し・要件8.3/7.3）。
 * 現在内容を退避してからベースライン内容を返す（退避不能はエラーで reject）。
 */
export function rollbackFile(path: string): Promise<string> {
  return invoke<string>("rollback_file", { path });
}

/** プレビュー系統（src-tauri preview::PreviewMode と対応・要件6.1/6.3）。 */
export type PreviewMode = "markdown" | "html";

/** prepare_preview の戻り（src-tauri preview::PreparedPreview と対応・design doc 6章）。 */
export interface PreparedPreview {
  /** 別WebView へナビゲートする URL（pika-preview:// 経由）。HTML 本体は乗らない。 */
  url: string;
  /** 占有世代（タブ/モード/差分の切替直列化）。 */
  generation: number;
  /** 系統A の信頼 JS 注入に使う nonce（系統B では空）。 */
  nonce: string;
  /** 系統（"markdown" | "html"）。 */
  flavor: string;
}

/**
 * 文書をプレビュー用に準備し、別WebView へ流す URL を得る（要件6・design doc 6章）。
 * HTML 本体は invoke の戻り値に乗らず、別WebView が custom protocol(pika-preview://)で取得する。
 */
export function preparePreview(
  path: string,
  mode: PreviewMode,
  content: string,
  allowExternal?: string[],
): Promise<PreparedPreview> {
  return invoke<PreparedPreview>("prepare_preview", {
    path,
    mode,
    content,
    allowExternal: allowExternal ?? null,
  });
}

/** 系統B（HTML）プレビューの危険検知結果（src-tauri preview::HtmlHazards と対応・要件6.3）。 */
export interface HtmlHazards {
  /** `<script>` を含む（JS は無効化済みだが「表示が崩れる」通知）。 */
  has_script: boolean;
  /** 外部 http(s) リソース参照を含む（既定遮断・オプトイン許可導線）。 */
  has_external_ref: boolean;
  /** `<meta http-equiv="refresh">` を含む（除去済みだが通知）。 */
  has_meta_refresh: boolean;
}

/** HTML プレビューの危険（script/外部参照/meta refresh）を検知する（要件6.3）。 */
export function scanHtmlHazards(content: string): Promise<HtmlHazards> {
  return invoke<HtmlHazards>("scan_html_hazards", { content });
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
