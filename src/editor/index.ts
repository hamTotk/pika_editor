// CodeMirror 6 エディタ配線（design doc 5章）。
// 外部リロード=単一トランザクション1Undo境界・非dirty は sprint 2/6 で結線する。
// 本スプリント（最薄ループ）は CM6 を実体として開き・編集・内容取得までを成立させる。
import { EditorState, type Extension } from "@codemirror/state";
import { EditorView, keymap, lineNumbers, highlightActiveLine } from "@codemirror/view";
import { defaultKeymap, history, historyKeymap } from "@codemirror/commands";
import { markdown } from "@codemirror/lang-markdown";

export interface EditorHandle {
  /** 現在のバッファ内容を取得する（保存に使う）。 */
  getContent(): string;
  /** 内容を差し替える（タブ切替時）。 */
  setContent(text: string): void;
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
          if (update.docChanged) onChange();
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
    destroy: () => view.destroy(),
  };
}
