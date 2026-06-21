// CodeMirror 6 エディタ配線（design doc 5章・要件5.1/7.2）。
// 外部リロード=単一トランザクション=1Undo境界・非dirty・スクロール/カーソル維持を結線する。
import { EditorState, type Extension, Annotation } from "@codemirror/state";
import { EditorView, keymap, lineNumbers, highlightActiveLine } from "@codemirror/view";
import { defaultKeymap, history, historyKeymap } from "@codemirror/commands";
import { markdown } from "@codemirror/lang-markdown";

/** 外部リロードのトランザクションに付ける注釈。これが付いた変更は dirty 化しない（要件7.2/5.1）。 */
const ExternalReload = Annotation.define<boolean>();

export interface EditorHandle {
  /** 現在のバッファ内容を取得する（保存に使う）。 */
  getContent(): string;
  /** 内容を差し替える（タブ切替時）。 */
  setContent(text: string): void;
  /**
   * 外部変更を反映する（要件7.2）。単一トランザクション=1Undo境界で内容を差し替え、
   * dirty にせず、カーソル/スクロール位置を可能な範囲で維持する。
   * リロード 1 回を 1 つの取り消し単位として Ctrl+Z で直前の自分の状態へ戻せる（要件5.1）。
   */
  reloadExternal(text: string): void;
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
 */
export function createEditor(
  parent: HTMLElement,
  initialDoc: string,
  onChange: () => void,
): EditorHandle {
  parent.replaceChildren();

  const view = new EditorView({
    parent,
    state: EditorState.create({
      doc: initialDoc,
      extensions: [
        ...baseExtensions,
        EditorView.updateListener.of((update) => {
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
    destroy: () => view.destroy(),
  };
}
