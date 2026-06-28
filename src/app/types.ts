// アプリシェルの共有型（S8・main.ts モノリス分割の土台）。
//
// main.ts と抽出モジュール（app/persistence・ui/menu-specs・ui/tree-actions）が同じ `OpenTab` /
// アプリ状態の型を参照できるよう、main.ts から型定義のみをここへ移した（挙動不変・実体は main が持つ）。
// 抽出モジュールへは main がこの `AppShellState`（state 本体への参照）とコールバックを注入する。

import type { TabModel } from "../ui/tabs";
import type { EditorHandle } from "../editor";
import type { DiffHandle } from "../diff";
import type { UnreadStore } from "../ui/unread";
import type { ViewMode, PreviewSerializer } from "../preview";
import type { TreeEntry, Settings, DocEncoding, LineEnding } from "../ipc";

export interface OpenTab extends TabModel {
  dirty: boolean;
  /**
   * このタブの直近既知内容ハッシュ（LF 正規化・backend と同一規則）。
   * 開く/保存/外部リロード時に backend(hash_content)で実値を詰め、終了時 collectAppState で
   * state.json の content_hash として保存する。これにより復元の「別物=未読復元」分岐が
   * production で発火する（eval high: ダミー値固定の解消）。
   */
  contentHash: string;
  /** 1 始まりカーソル位置（非アクティブタブの復元用に最後に見えた位置を保持）。 */
  cursorLine: number;
  cursorColumn: number;
  /** スクロール最上部の行番号（1 始まり近似・復元用）。 */
  scrollTop: number;
  /**
   * 起動復元時に外部削除されていたタブ（取消線＋× 表示で残す）。退避/ベースラインは snapshot に
   * 残るので、削除済みタブから「確認済み時点に戻す（rollback）」へ到達できる回復導線を保つ
   * （eval high: 削除済みタブの回復導線欠落＝旧 wx 版 F-017 と同質の行き止まり防止）。
   */
  deleted: boolean;
  /**
   * 非テキスト（画像/非対応バイナリ＝要件12.2・U3）か。true のとき CM6 を作らず image-host で表示する
   * （画像は簡易ビュー、巨大画像/非対応は「既定アプリで開く」誘導）。モード/差分は無効・カーソルステータス無し。
   */
  nonText: boolean;
  /** 第2段階以降（編集不可・読み取り専用ビューア）か。開き直しメニューの無効化に使う（backend degrade.editing_off を保存）。 */
  editingOff: boolean;
  /**
   * 開いたときに検出した元エンコーディング（保存時に維持する＝要件5.2・eval medium）。
   * open_document が判定した値。これを save_document に渡し、Shift_JIS 等が暗黙に UTF-8 化
   * されるのを防ぐ（最上位原則「データを失わない」）。新規タブ/不明時は utf-8。
   */
  encoding: DocEncoding;
  /** 開いたときに BOM があったか（保存時に維持する＝要件5.2）。 */
  hasBom: boolean;
  /**
   * 開いたときに検出した改行コード（表示メニューの現在値表示用・要件5.2）。
   * 変換 backend は未実装のため表示専用（要件14章「足さない」）。新規/不明時は "none"。
   */
  lineEnding: LineEnding;
  /**
   * このタブで「許可して再読込」したオプトイン外部許可ホスト（要件6.2/6.3・2.4）。
   * 既定は undefined（外部遮断）。許可は **タブ単位** で保持し、別タブ/別文書では既定オフに戻る
   * （永続はしない＝要件6.2「既定は必ずオフに戻る」）。renderActivePreview がこれを buildPreview へ渡す。
   */
  allowExternal?: string[];
  /**
   * 未保存編集の保持バッファ（eval high #11・最上位原則「データを失わない」）。
   *
   * タブは単一の CM6（state.editor）を共有し、タブ切替のたびに editor を destroy→再生成する。
   * このため dirty タブから別タブへ切り替えると、退避先が無ければ編集中テキストが消える。さらに
   * 再アクティブ化時に activateTab が openDocument でディスク内容を読み直して上書きすると未保存編集を
   * 失う。これを防ぐため、**dirty タブから切り替える瞬間に編集中テキストをここへ退避**し、再アクティブ化
   * では（dirty なら）ディスクでなくこの draft を CM6 へ載せる。保存（onSave）成功で dirty=false に
   * なったら draft はクリアする（以後はディスク内容を正とする）。非 dirty タブは常に undefined。
   */
  draft?: string;
}

/**
 * メインWebView シェルの可変状態（main.ts の `state` の型）。
 *
 * 抽出モジュールはこのオブジェクトへの**参照**を注入され、main.ts と同じ単一の可変状態を読み書きする
 * （挙動不変＝データ損失防止の不変条件を崩さない）。フィールドの意味づけは main.ts の生成箇所のコメント参照。
 */
export interface AppShellState {
  tabs: OpenTab[];
  active: string | null;
  editor: EditorHandle | null;
  treeEntries: TreeEntry[];
  unread: UnreadStore;
  folder: string | null;
  diffOn: boolean;
  diff: DiffHandle | null;
  viewMode: ViewMode;
  previewSerializer: PreviewSerializer;
  busy: boolean;
  safeEmpty: boolean;
  lineWrapping: boolean;
  tabWidth: number;
  settings: Settings | null;
}
