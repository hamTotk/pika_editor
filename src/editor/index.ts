// CodeMirror 6 エディタ配線（design doc 5章・要件5.1/7.2）。
// 外部リロード=単一トランザクション=1Undo境界・非dirty・スクロール/カーソル維持を結線する。
import {
  EditorState,
  type Extension,
  Annotation,
  Compartment,
  StateField,
  StateEffect,
} from "@codemirror/state";
import {
  EditorView,
  keymap,
  lineNumbers,
  highlightActiveLine,
  Decoration,
  type DecorationSet,
} from "@codemirror/view";
import { defaultKeymap, history, historyKeymap, indentWithTab } from "@codemirror/commands";
import { markdown } from "@codemirror/lang-markdown";
import { HighlightStyle, syntaxHighlighting, indentUnit } from "@codemirror/language";
import { tags } from "@lezer/highlight";

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
  /**
   * タブの**表示幅**（EditorState.tabSize）を動的に切替える（settings.toml の tab_width 反映・要件10.3/5.2）。
   * これは Tab 文字を画面上で何桁ぶん詰めて見せるかの設定であり、挿入文字（タブ文字）は変えない。
   * setLineWrapping と全く同じく Compartment で差し替えるだけなので、内容/カーソル/スクロール/履歴は維持される。
   */
  setTabWidth(n: number): void;
  /**
   * 検索ヒットのハイライトを差し替える（U4・要件5.4）。位置は **UTF-16 コードユニット**（CM6 ネイティブ座標系）。
   * 呼び出し側は backend の utf16_start/utf16_end を {from,to} へ写して渡す。current は現在ヒットの index
   * （-1 で「現在」なし）。空配列でハイライトをクリアする。
   */
  setSearchMatches(matches: { from: number; to: number }[], current: number): void;
  /**
   * 現在ヒット（UTF-16 区間）を選択し中央へスクロールする（U4・要件5.4）。
   * 置換（1件）の「現在位置」もこの選択で示す。
   */
  scrollToMatch(from: number, to: number): void;
  /** 検索ハイライトを消す（バーを閉じる/query 空/不正正規表現）。 */
  clearSearch(): void;
  /** エディタへフォーカスを戻す（検索バーで Esc を押したとき）。 */
  focusEditor(): void;
  /** 破棄する。 */
  destroy(): void;
}

/**
 * 控えめな構文ハイライト（UIブラッシュアップ T7・差分 E2・ui-mock .tok-* / ui-design 1章モノトーン基調）。
 * 色は**直書きせず** class を割り当て、色値は src/styles/app.css 側でトークン変数（--text-1 等）で当てる。
 * これにより html[data-theme] のライト/ダーク切替へ自動追従する（color 直書きだと追従できない）。
 * 構文色は彩度を落として主張させすぎない（差分や強調の色と競合させない）。
 */
const pikaHighlightStyle = HighlightStyle.define([
  // 見出し（tok-h 相当）= 太字＋text-1。
  { tag: tags.heading, class: "cm-tok-heading" },
  // 太字/強調はマークアップに準じて装飾のみ（色は付けずモノトーン基調を保つ）。
  { tag: tags.strong, class: "cm-tok-strong" },
  { tag: tags.emphasis, class: "cm-tok-emphasis" },
  // キーワード（tok-k 相当）= accent 系（トークンで light/dark を吸収）。
  { tag: tags.keyword, class: "cm-tok-keyword" },
  // 文字列（tok-s 相当）= 落ち着いた色（専用変数が無いので app.css で light/dark 個別指定）。
  { tag: tags.string, class: "cm-tok-string" },
  // コメント（tok-c 相当）= text-3・italic。行/ブロックコメントも同扱い。
  { tag: [tags.comment, tags.lineComment, tags.blockComment], class: "cm-tok-comment" },
  // インラインコード/等幅は等幅のまま（淡背景は付けすぎない）。
  { tag: tags.monospace, class: "cm-tok-monospace" },
  // リンク/URL は accent（控えめ）。
  { tag: [tags.link, tags.url], class: "cm-tok-link" },
]);

/**
 * 検索ヒットのハイライトを差し替える StateEffect（U4・要件5.4）。
 *
 * `@codemirror/search` は使わず自前の StateField+Decoration で全ヒットを薄く、現在ヒット 1 件を
 * 強く色付ける（CM6 既定検索 keymap と競合させない＝design 方針）。位置は **UTF-16 コードユニット**
 * （CM6 ネイティブの座標系）で受け取る。呼び出し側（search モジュール）が backend の utf16_start/utf16_end
 * をそのまま渡す。current は現在ヒットの index（-1 で「現在」無し＝全件を薄表示のみ）。
 */
const setSearchMatchesEffect = StateEffect.define<{
  matches: { from: number; to: number }[];
  current: number;
}>();

/** 全ヒット用デコレーション（薄い強調）。色は app.css の .cm-search-hit が当てる（テーマ追従）。 */
const searchHitMark = Decoration.mark({ class: "cm-search-hit" });
/** 現在ヒット用デコレーション（強い強調）。.cm-search-hit-current を重ねる。 */
const searchHitCurrentMark = Decoration.mark({
  class: "cm-search-hit cm-search-hit-current",
});

/**
 * 検索ヒットのデコレーション集合を保持する StateField（U4・要件5.4）。
 * effect を受けるたびに matches から組み直す。空配列でクリアする（query 変更/閉じる/不正正規表現）。
 * CM6 は Decoration.set に **from<to の昇順** を要求するため呼び出し側で並べ替えて渡す前提だが、
 * 念のためここでも from 昇順へソートしてから set する（順序崩れによる例外を防ぐ）。
 */
const searchHighlightField = StateField.define<DecorationSet>({
  create() {
    return Decoration.none;
  },
  update(deco, tr) {
    let next = deco;
    // 編集に追従して既存ハイライトを写像する（手動編集中は古くなりうるが例外で落ちないよう map する）。
    if (tr.docChanged) next = next.map(tr.changes);
    for (const effect of tr.effects) {
      if (!effect.is(setSearchMatchesEffect)) continue;
      const { matches, current } = effect.value;
      if (matches.length === 0) {
        next = Decoration.none;
        continue;
      }
      // from 昇順に整える（CM6 の Decoration.set は昇順必須）。
      const sorted = matches
        .map((m, i) => ({ ...m, isCurrent: i === current }))
        .sort((a, b) => a.from - b.from);
      const ranges = sorted.map((m) =>
        (m.isCurrent ? searchHitCurrentMark : searchHitMark).range(m.from, m.to),
      );
      // 第2引数 true = ソート済みを宣言（昇順を保証して O(n) で構築する）。
      next = Decoration.set(ranges, true);
    }
    return next;
  },
  provide: (field) => EditorView.decorations.from(field),
});

const baseExtensions: Extension[] = [
  lineNumbers(),
  highlightActiveLine(),
  history(),
  markdown(),
  // 検索ハイライト（自前 StateField・要件5.4・U4）。空集合で開始（バー未起動時は何も描かない）。
  searchHighlightField,
  // 控えめな構文ハイライトを有効化（class 指定＝テーマ追従・色は app.css）。
  syntaxHighlighting(pikaHighlightStyle),
  // Tab はタブ文字を挿入する（スペース展開しない・保守的既定＝要件5.2）。indentUnit を "\t" に固定し、
  // indentWithTab で Tab/Shift+Tab をインデント操作へ割り当てる。tab_width は**表示幅にのみ**効く値で、
  // ここで入るのは常にタブ文字 1 個（挿入スペース数ではない）。indentWithTab は defaultKeymap より
  // 先に置き、フォーカスがエディタにある間 Tab がインデントへ向かうようにする。
  indentUnit.of("\t"),
  keymap.of([indentWithTab, ...defaultKeymap, ...historyKeymap]),
];

/**
 * 指定ホスト要素に CM6 を生成する。
 * @param parent エディタを描画する DOM 要素
 * @param initialDoc 初期内容
 * @param onChange 編集（dirty 化）通知。保存ボタンの活性に使う
 * @param onCursorChange カーソル/選択/内容の変化通知。右下ステータスの追従更新に使う（要件11.1）。
 *   selectionSet（カーソル移動・選択）または docChanged（編集）のたびに発火する。
 * @param lineWrapping 初期の折り返し ON/OFF（表示メニューのトグルが保持する現在値・既定 ON）。
 *   既定 ON で短文ファイルの不要な横スクロールバーを避ける（ui-design §120）。
 *   タブ切替でエディタを作り直しても現在の折り返し設定を引き継ぐために初期値で渡す。
 * @param tabWidth タブの**表示幅**（EditorState.tabSize・settings.toml の tab_width・既定 4）。
 *   挿入文字（タブ文字）には影響せず、Tab 文字を画面上で何桁ぶんに見せるかだけを決める（要件5.2）。
 *   lineWrapping と同じく、タブ切替でエディタを作り直しても現在値を初期値で引き継ぐ。
 */
export function createEditor(
  parent: HTMLElement,
  initialDoc: string,
  onChange: () => void,
  onCursorChange?: () => void,
  lineWrapping = true,
  tabWidth = 4,
): EditorHandle {
  parent.replaceChildren();

  // 折り返しを動的に差し替えるための Compartment（setLineWrapping で reconfigure する）。
  const wrapCompartment = new Compartment();
  // タブ表示幅を動的に差し替えるための Compartment（setTabWidth で reconfigure する）。
  // wrapCompartment と同じ作法で、内容/カーソル/履歴を壊さず tab_width 変更を反映する。
  const tabSizeCompartment = new Compartment();

  const view = new EditorView({
    parent,
    state: EditorState.create({
      doc: initialDoc,
      extensions: [
        ...baseExtensions,
        wrapCompartment.of(lineWrapping ? EditorView.lineWrapping : []),
        // タブ表示幅（EditorState.tabSize の Facet）を Compartment 経由で初期化する（setTabWidth で差替）。
        tabSizeCompartment.of(EditorState.tabSize.of(tabWidth)),
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
          "&": {
            height: "100%",
            fontSize: "13px",
            // エディタ全体の地色・文字色をトークンへ追従させる（ダーク時に CM6 既定 light が残らない）。
            backgroundColor: "var(--bg-raised)",
            color: "var(--text-1)",
          },
          ".cm-scroller": {
            fontFamily: "var(--font-mono)",
            // 折り返し既定 ON では横スクロールバーは不要。折り返し OFF（表示メニュー）に切替えた
            // ときだけ CM6 が自前で横スクロールを出す（lineWrapping 拡張が overflow を制御する）。
            // 短文ファイルで常時出ていた横スクロールバーをここで明示的に抑止する（item1）。
            overflowX: "auto",
          },
          // 行番号ガター（item4・ui-design §2）。ダークで白く残るのは CM6 既定 light テーマ由来。
          // 背景は sunken（脇役を沈める）・文字は text-3（淡色・常設だが numbers は弁別用途）へ揃え、
          // html[data-theme] のライト/ダーク切替へ CSS 変数で自動追従させる。
          ".cm-gutters": {
            backgroundColor: "var(--bg-sunken)",
            color: "var(--text-3)",
            border: "none",
            borderRight: "1px solid var(--border-1)",
          },
          ".cm-lineNumbers .cm-gutterElement": {
            color: "var(--text-3)",
          },
          // アクティブ行のガター/本体は薄掛け（bg-active）でモノトーン基調を保つ。
          ".cm-activeLineGutter": {
            backgroundColor: "var(--bg-active)",
            color: "var(--text-2)",
          },
          ".cm-activeLine": {
            backgroundColor: "var(--bg-hover)",
          },
          // カーソル/選択もトークンへ追従（ダークで視認できるよう accent/bg-active を当てる）。
          ".cm-cursor, .cm-dropCursor": {
            borderLeftColor: "var(--accent)",
          },
          "&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection": {
            backgroundColor: "var(--bg-active)",
          },
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
    setTabWidth: (n: number) => {
      // tabSize Facet を Compartment 経由で差し替える＝内容/カーソル/スクロール/履歴は保持される
      //（setLineWrapping と同じ作法）。表示幅のみ変わり挿入文字（タブ文字）は変わらない。
      view.dispatch({
        effects: tabSizeCompartment.reconfigure(EditorState.tabSize.of(n)),
      });
    },
    setSearchMatches: (matches: { from: number; to: number }[], current: number) => {
      // UTF-16 座標の matches をそのまま StateField へ流す（CM6 はこの座標系でデコレーション/選択を扱う）。
      view.dispatch({ effects: setSearchMatchesEffect.of({ matches, current }) });
    },
    scrollToMatch: (from: number, to: number) => {
      // 現在ヒットを選択し中央へ。選択は置換（1件）の「現在位置」表示も兼ねる。
      view.dispatch({
        selection: { anchor: from, head: to },
        effects: EditorView.scrollIntoView(from, { y: "center" }),
      });
    },
    clearSearch: () => {
      view.dispatch({ effects: setSearchMatchesEffect.of({ matches: [], current: -1 }) });
    },
    focusEditor: () => view.focus(),
    destroy: () => view.destroy(),
  };
}
