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

/** パスの種別を返す（存在しないファイルを新規タブで開く判断に使う・要件3.2）。 */
export function pathKind(path: string): Promise<"file" | "dir" | "missing"> {
  return invoke<"file" | "dir" | "missing">("path_kind", { path });
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

/** 系統B（HTML）プレビューの危険検知結果（src-tauri preview::HtmlHazards と対応・要件6.3）。 */
export interface HtmlHazards {
  /** `<script>` を含む（JS は無効化済みだが「表示が崩れる」通知）。 */
  has_script: boolean;
  /** 外部 http(s) リソース参照を含む（既定遮断・オプトイン許可導線）。 */
  has_external_ref: boolean;
  /** `<meta http-equiv="refresh">` を含む（除去済みだが通知）。 */
  has_meta_refresh: boolean;
}

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
  /** 系統B の危険検知（要件6.3・通知バー導線）。content の二重 invoke を避けここに同梱。系統A は全 false。 */
  hazards: HtmlHazards;
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

/** 表示モード（state.json の ViewMode と対応・ui-design 8章）。 */
export type ViewMode = "source" | "preview" | "split";

/** 1 タブ分の永続状態（pika-core::state::TabState と対応・要件10.1）。 */
export interface TabState {
  path: string;
  cursor_line: number;
  cursor_column: number;
  scroll_top: number;
  view_mode: ViewMode;
  diff_on: boolean;
  /** 保存時点の LF 正規化内容ハッシュ（復元時の別物=未読判定に使う）。 */
  content_hash: string;
}

/** ウィンドウ状態（pika-core::state::WindowState と対応）。 */
export interface WindowState {
  x: number;
  y: number;
  width: number;
  height: number;
  maximized: boolean;
}

/** 最近使った項目（pika-core::recent::RecentList と対応・要件10.2）。 */
export interface RecentList {
  files: string[];
  folders: string[];
}

/** アプリ状態（pika-core::state::AppState と対応・要件10.1）。 */
export interface AppState {
  version: number;
  workspace?: string;
  tabs: TabState[];
  active_tab: number;
  expanded_dirs: string[];
  window: WindowState;
  recent: RecentList;
}

/** 復元したタブ 1 件（src-tauri commands::RestoredTabDto と対応・状態復元3分岐）。 */
export interface RestoredTab {
  /** "restore"（正常）| "deleted"（消失=削除済み表示）| "unread"（別物=未読復元） */
  status: "restore" | "deleted" | "unread";
  tab: TabState;
}

/** 復元結果（src-tauri commands::RestoreOutcomeDto と対応・要件10.1/13）。 */
export interface RestoreOutcome {
  /** "restore" | "empty-state"（ワークスペース消失）| "no-workspace"（単体ファイルのみ） */
  workspace_status: "restore" | "empty-state" | "no-workspace";
  workspace_path?: string;
  tabs: RestoredTab[];
  /** 復元後にアクティブ化すべきタブの絶対パス（active_tab を解決した値・無ければ未設定）。 */
  active_path?: string;
  /** 未知バージョン/破損で空起動した（このセッションは上書き保存を控える）。 */
  safe_empty: boolean;
}

/** アプリ状態を state.json へアトミック保存する（要件10.1・design doc 9章）。 */
export function saveAppState(state: AppState): Promise<void> {
  return invoke<void>("save_app_state", { state });
}

/** アプリ状態を state.json から復元する（version 安全側・復元3分岐は backend が判定）。 */
export function restoreAppState(): Promise<RestoreOutcome> {
  return invoke<RestoreOutcome>("restore_app_state");
}

/**
 * 内容の LF 正規化ハッシュを得る（state.json のタブ content_hash 算出・要件10.1）。
 * 自己保存抑制/復元の別物判定と同一規則（pika_core::hashing）。改行のみの差は同値。
 */
export function hashContent(content: string): Promise<string> {
  return invoke<string>("hash_content", { content });
}

/**
 * 最近使った項目（ファイル/フォルダ）を追記し更新後リストを得る（要件10.2・ジャンプリスト）。
 * LRU・重複排除・上限20件は backend(pika-core::recent)。未知バージョン/破損時は空を返す（保全）。
 */
export function noteRecent(kind: "file" | "folder", path: string): Promise<RecentList> {
  return invoke<RecentList>("note_recent", { kind, path });
}

/** 単一インスタンス転送の「これを開け」イベント（src-tauri single_instance::OpenRequestPayload と対応）。 */
export interface OpenRequestPayload {
  /** 再検証済み絶対パス群（受理操作=パスオープン限定）。 */
  paths: string[];
  /** `-g` カーソル位置（任意）。 */
  goto?: { line: number; column?: number };
}

/** 単一インスタンス転送（pika <path> を再実行）で送られる「開け」要求の購読（要件3.4）。 */
export function onOpenRequest(
  handler: (payload: OpenRequestPayload) => void,
): Promise<UnlistenFn> {
  return listen<OpenRequestPayload>("open-request", (e) => handler(e.payload));
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

// ── sprint 6: 巨大ファイル段階制・エンコーディング・検索置換 ──────────────────

/** 巨大ファイル段階（要件2.2・pika-core::huge::FileStage と対応）。 */
export type FileStage = "normal" | "stage1" | "stage2" | "too-large";

/** 段階制の縮退フラグ（自動オフされた機能・通知バー提示用・要件2.2/11.1）。 */
export interface DegradeFlags {
  preview_off: boolean;
  diff_off: boolean;
  baseline_content_off: boolean;
  highlight_off: boolean;
  wrap_off: boolean;
  editing_off: boolean;
}

/** 改行分類（表示メニュー用・要件5.2）。 */
export type LineEnding = "lf" | "crlf" | "cr" | "mixed" | "none";

/** エンコーディング識別子（保存時に維持する・要件5.2）。 */
export type DocEncoding = "utf-8" | "utf-16le" | "utf-16be" | "shift_jis";

/** open_document の戻り（src-tauri document::OpenedDocument と対応・要件2.2/5.2）。 */
export interface OpenedDocument {
  stage: FileStage;
  degrade: DegradeFlags;
  /** デコード済みテキスト（第2段階以降/上限超では空＝仮想化ビューア or エラー）。 */
  text: string;
  encoding: DocEncoding;
  has_bom: boolean;
  line_ending: LineEnding;
  /** 自動判定に失敗し UTF-8 警告で開いたか（要件5.2）。 */
  decode_warning: boolean;
  size_bytes: number;
}

/**
 * 文書を開く（巨大ファイル段階制＋エンコーディング自動判定・要件2.2/5.2）。
 * 第2段階以降は内容を返さず段階だけ返す（仮想化ビューアは readRange で読む）。
 */
export function openDocument(path: string): Promise<OpenedDocument> {
  return invoke<OpenedDocument>("open_document", { path });
}

/** 表現不能文字 1 件（［該当文字を確認］のジャンプ用・要件5.6）。 */
export interface UnmappableChar {
  ch: string;
  char_index: number;
}

/** save_document の結果（src-tauri document::SaveResultDto と対応・要件5.2/5.6）。 */
export interface SaveResult {
  /** "saved" = 保存完了 / "unmappable" = 表現不能文字で中断（選択肢提示）。 */
  status: "saved" | "unmappable";
  /** 中断時の表現不能文字（［UTF-8で保存/該当文字を確認/キャンセル］の「確認」用）。 */
  unmappable: UnmappableChar[];
}

/**
 * エンコーディングを維持して保存する（要件5.2/5.6）。表現不能文字があれば保存せず中断する。
 * forceUtf8=true は［UTF-8で保存］選択肢（BOM なし UTF-8 で書き出す）。
 */
export function saveDocument(
  path: string,
  content: string,
  encoding: DocEncoding,
  hasBom: boolean,
  forceUtf8: boolean,
): Promise<SaveResult> {
  return invoke<SaveResult>("save_document", {
    path,
    content,
    encoding,
    hasBom,
    forceUtf8,
  });
}

/** 仮想化ビューアの 1 ウィンドウ（src-tauri document::RangeWindowDto と対応・要件2.2 第2段階）。 */
export interface RangeWindow {
  start: number;
  end: number;
  size_bytes: number;
  /** 範囲のテキスト（読み取り専用ビューア表示用）。 */
  text: string;
}

/**
 * 巨大ファイル（第2段階）のバイト範囲を読み行境界に整えて返す（要件2.2 第2段階・design doc 8章）。
 * CM6 に全量ロードせず要求位置近傍の 1 ウィンドウだけを読む（読み取り専用）。
 */
export function readRange(
  path: string,
  center: number,
  window?: number,
): Promise<RangeWindow> {
  return invoke<RangeWindow>("read_range", { path, center, window: window ?? null });
}

/** 検索オプション（要件5.4: 大文字小文字区別・単語単位・正規表現）。 */
export interface SearchOptions {
  case_sensitive: boolean;
  whole_word: boolean;
  regex: boolean;
}

/** 検索 1 ヒット（バイトオフセット半開区間・要件5.4）。 */
export interface SearchMatch {
  start: number;
  end: number;
}

/** 検索結果（src-tauri document::SearchResultDto と対応・要件5.4）。 */
export interface SearchResult {
  matches: SearchMatch[];
  /** 件数上限で打ち切られたか。 */
  truncated: boolean;
  /** キャンセルで打ち切られたか。 */
  cancelled: boolean;
}

/**
 * テキスト内検索（要件5.4: 全ヒット・件数・後方参照/キャプチャ参照/Unicode 文字クラス）。
 * 新しい検索を始めると直前の検索はキャンセルされる（固まらない）。
 */
export function searchInText(
  text: string,
  query: string,
  options: SearchOptions,
): Promise<SearchResult> {
  return invoke<SearchResult>("search_in_text", { text, query, options });
}

/** 置換結果（src-tauri document::ReplaceResultDto と対応・要件5.4）。 */
export interface ReplaceResult {
  text: string;
  replaced: number;
  truncated: boolean;
  cancelled: boolean;
}

/**
 * テキスト全置換（要件5.4: 正規表現置換・キャプチャ参照 $1/${name}）。
 * 差分は読み取り専用・第2段階は置換無効（呼び出し側が段階で活性制御する）。
 */
export function replaceInText(
  text: string,
  query: string,
  replacement: string,
  options: SearchOptions,
): Promise<ReplaceResult> {
  return invoke<ReplaceResult>("replace_in_text", {
    text,
    query,
    replacement,
    options,
  });
}

/**
 * 診断ログフォルダの絶対パスを取得する（要件12.3「メニューからログフォルダを開ける」）。
 * バックエンドがフォルダ（<データルート>/logs/）を作成してからパスを返す。
 * 実際にエクスプローラーで開くのは OS シェル（呼び出し側がこのパスを使う）。
 */
export function logFolderPath(): Promise<string> {
  return invoke<string>("log_folder_path");
}
