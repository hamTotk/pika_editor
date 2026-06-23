// CodeMirror 6 エディタ配線（design doc 5章・要件5.1/7.2）。
// 外部リロード=単一トランザクション=1Undo境界・非dirty・スクロール/カーソル維持を結線する。
import { EditorState, type Extension, Annotation, Compartment } from "@codemirror/state";
import { EditorView, keymap, lineNumbers, highlightActiveLine } from "@codemirror/view";
import { defaultKeymap, history, historyKeymap } from "@codemirror/commands";
import { markdown } from "@codemirror/lang-markdown";

/** 外部リロードのトランザクションに付ける注釈。これが付いた変更は dirty 化しない（要件7.2/5.1）。 */
const ExternalReload = Annotation.define<boolean>();

/** カーソル位置（1 始まり行・桁。state.json 保存用）。 */
export interface CursorPos {
  line: number;
  column: number;
}

/** ステータス表示用のエディタ計測値（行数・文字数・カーソル・選択文字数＝要件11.1）。 */
export interface EditorMetrics {
  /** 総行数。 */
  lines: number;
  /** 総文字数（CM6 の doc.length＝コードユニット長）。 */
  chars: number;
  /** カーソル位置（1 始まり行・桁）。 */
  cursorLine: number;
  cursorColumn: number;
  /** 選択中の文字数（未選択は 0）。 */
  selectionChars: number;
}

export interface EditorHandle {
  /** 現在のバッファ内容を取得する（保存に使う）。 */
  getContent(): string;
  /** 現在のカーソル位置（1 始まり行・桁）を取得する（state.json 保存用・要件10.1）。 */
  getCursor(): CursorPos;
  /** ステータス表示用の計測値（行数・文字数・カーソル・選択文字数）を取得する（要件11.1）。 */
  getMetrics(): EditorMetrics;
  /** スクロール位置（先頭の表示行番号 1 始まり近似）を取得する（state.json 保存用・要件10.1）。 */
  getScrollTop(): number;
  /** 内容を差し替える（タブ切替時）。 */
  setContent(text: string): void;
  /**
   * 外部変更を反映する（要件7.2）。単一トランザクション=1Undo境界で内容を差し替え、
   * dirty にせず、カーソル/スクロール位置を可能な範囲で維持する。
   * リロード 1 回を 1 つの取り消し単位として Ctrl+Z で直前の自分の状態へ戻せる（要件5.1）。
   */
  reloadExternal(text: string): void;
  /**
   * 指定位置へカーソルを置く（`-g <file>:<行>[:<桁>]` の goto・要件3.1）。
   * 行・桁は 1 始まり。行数超過は最終行、桁省略/超過は行頭/行末へクランプする。
   */
  gotoPosition(line: number, column: number): void;
  /**
   * 指定行が最上部に来るようスクロールする（state.json 復元用・要件10.1）。
   * カーソルは動かさない（カーソル復元は gotoPosition が担う）。行超過は最終行へクランプ。
   */
  scrollToLine(line: number): void;
  /**
   * 行の折り返し（lineWrapping）を動的に切替える（表示メニューの「折り返し」トグル・UIブラッシュアップ T8）。
   * Compartment で差し替えるのでエディタは作り直さず、内容/カーソル/スクロール/履歴は維持される
   *（外部リロード非dirty・カーソル維持の既存挙動を壊さない）。
   */
  setLineWrapping(on: boolean): void;
  /** 破棄する。 */
  destroy(): void;
}

const baseExtensions: Extension[] = [
  lineNumbers(),
  highlightActiveLine(),
  history(),
  markdown(),
  keymap.of([...defaultKeymap, ...historyKeymap]),
];

/**
 * 指定ホスト要素に CM6 を生成する。
 * @param parent エディタを描画する DOM 要素
 * @param initialDoc 初期内容
 * @param onChange 編集（dirty 化）通知。保存ボタンの活性に使う
 * @param onCursorChange カーソル/選択/内容の変化通知。右下ステータスの追従更新に使う（要件11.1）。
 *   selectionSet（カーソル移動・選択）または docChanged（編集）のたびに発火する。
 * @param lineWrapping 初期の折り返し ON/OFF（表示メニューのトグルが保持する現在値・既定 false）。
 *   タブ切替でエディタを作り直しても現在の折り返し設定を引き継ぐために初期値で渡す。
 */
export function createEditor(
  parent: HTMLElement,
  initialDoc: string,
  onChange: () => void,
  onCursorChange?: () => void,
  lineWrapping = false,
): EditorHandle {
  parent.replaceChildren();

  // 折り返しを動的に差し替えるための Compartment（setLineWrapping で reconfigure する）。
  const wrapCompartment = new Compartment();

  const view = new EditorView({
    parent,
    state: EditorState.create({
      doc: initialDoc,
      extensions: [
        ...baseExtensions,
        wrapCompartment.of(lineWrapping ? EditorView.lineWrapping : []),
        EditorView.updateListener.of((update) => {
          // カーソル移動（selectionSet）または編集（docChanged）でステータスを追従させる（要件11.1）。
          // 外部リロードも選択/内容が変わるので拾い、新しい行数・文字数・位置を反映させる。
          if (update.selectionSet || update.docChanged) onCursorChange?.();
          // 外部リロード由来の変更は dirty 化しない（要件7.2: リロードは dirty にしない）。
          if (!update.docChanged) return;
          const isExternal = update.transactions.some((tr) => tr.annotation(ExternalReload));
          if (!isExternal) onChange();
        }),
        EditorView.theme({
          "&": { height: "100%", fontSize: "13px" },
          ".cm-scroller": { fontFamily: "var(--font-mono)" },
        }),
      ],
    }),
  });

  return {
    getContent: () => view.state.doc.toString(),
    getCursor: () => {
      // メインカーソル（selection.main.head）の絶対位置から 1 始まり行・桁へ換算する。
      const head = view.state.selection.main.head;
      const line = view.state.doc.lineAt(head);
      return { line: line.number, column: head - line.from + 1 };
    },
    getMetrics: () => {
      const doc = view.state.doc;
      const sel = view.state.selection.main;
      const line = doc.lineAt(sel.head);
      return {
        lines: doc.lines,
        chars: doc.length,
        cursorLine: line.number,
        cursorColumn: sel.head - line.from + 1,
        // 選択範囲の文字数（複数選択は主選択のみ・単純な end-start＝十分な近似）。
        selectionChars: Math.abs(sel.to - sel.from),
      };
    },
    getScrollTop: () => {
      // スクロール最上部に対応する行番号（1 始まり近似）。ピクセルでなく行で保持し、
      // 復元時に再オープン内容が多少変わっても破綻しにくくする（行ベース近似・要件10.1）。
      const topPos = view.lineBlockAtHeight(view.scrollDOM.scrollTop).from;
      return view.state.doc.lineAt(topPos).number;
    },
    setContent: (text: string) => {
      view.dispatch({
        changes: { from: 0, to: view.state.doc.length, insert: text },
      });
    },
    reloadExternal: (text: string) => {
      // カーソル/スクロールを保存し、単一トランザクションで内容差し替え後に復元する。
      const prevSel = view.state.selection.main;
      const scrollTop = view.scrollDOM.scrollTop;
      const len = text.length;
      view.dispatch({
        changes: { from: 0, to: view.state.doc.length, insert: text },
        // 行がずれた場合に備えて末尾を超えない位置へクランプ（近似維持・要件7.2）。
        selection: { anchor: Math.min(prevSel.anchor, len), head: Math.min(prevSel.head, len) },
        annotations: ExternalReload.of(true),
        scrollIntoView: false,
      });
      view.scrollDOM.scrollTop = scrollTop;
    },
    gotoPosition: (line: number, column: number) => {
      // 行数超過は最終行へクランプ（要件3.1: 行数超過は最終行）。1 始まり→CM6 の 1 始まり行。
      const lineCount = view.state.doc.lines;
      const targetLine = Math.max(1, Math.min(line, lineCount));
      const lineInfo = view.state.doc.line(targetLine);
      // 桁省略/超過は行頭/行末へクランプ（要件3.1: 桁省略は行頭）。
      const col = Math.max(1, column);
      const pos = Math.min(lineInfo.from + (col - 1), lineInfo.to);
      view.dispatch({ selection: { anchor: pos, head: pos }, scrollIntoView: true });
      view.focus();
    },
    scrollToLine: (line: number) => {
      const lineCount = view.state.doc.lines;
      const target = Math.max(1, Math.min(line, lineCount));
      const lineInfo = view.state.doc.line(target);
      // 指定行を上端に寄せる（カーソルは変えない＝復元時の位置近似）。
      view.dispatch({ effects: EditorView.scrollIntoView(lineInfo.from, { y: "start" }) });
    },
    setLineWrapping: (on: boolean) => {
      // Compartment を reconfigure するだけ＝内容/カーソル/スクロール/履歴は保持される。
      view.dispatch({
        effects: wrapCompartment.reconfigure(on ? EditorView.lineWrapping : []),
      });
    },
    destroy: () => view.destroy(),
  };
}
