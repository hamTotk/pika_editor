// CodeMirror 6 エディタ配線（design doc 5章・要件5.1/7.2）。
// 外部リロード=単一トランザクション=1Undo境界・非dirty・スクロール/カーソル維持を結線する。
import {
  EditorState,
  EditorSelection,
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
  type Command,
  drawSelection,
  rectangularSelection,
  crosshairCursor,
  highlightTrailingWhitespace,
  highlightSpecialChars,
} from "@codemirror/view";
import {
  defaultKeymap,
  history,
  historyKeymap,
  indentWithTab,
  selectParentSyntax,
} from "@codemirror/commands";
import { markdown, markdownLanguage } from "@codemirror/lang-markdown";
import { html, htmlLanguage } from "@codemirror/lang-html";
import { cssLanguage } from "@codemirror/lang-css";
import {
  javascriptLanguage,
  jsxLanguage,
  typescriptLanguage,
  tsxLanguage,
} from "@codemirror/lang-javascript";
import {
  HighlightStyle,
  syntaxHighlighting,
  indentUnit,
  bracketMatching,
  codeFolding,
  foldKeymap,
  foldService,
  type Language,
} from "@codemirror/language";
// @codemirror/search からは selectNextOccurrence(Ctrl+D)/selectSelectionMatches(Ctrl+Shift+L)/
// highlightSelectionMatches（同一語の薄い強調）だけを使う。searchKeymap / search パネル / search 拡張は
// import しない（pika 独自の Ctrl+F 検索バー＝自前 searchHighlightField と競合させない＝design 方針）。
import {
  selectNextOccurrence,
  selectSelectionMatches,
  highlightSelectionMatches,
} from "@codemirror/search";
import { tags, styleTags, Tag } from "@lezer/highlight";

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
  /**
   * 太字トグル（編集メニュー「太字」・Ctrl+B と同じ動作）。実行後エディタへフォーカスを戻す。
   * キーバインドが主経路で、メニューからの再利用のために公開する（コマンド本体は keymap と共有）。
   */
  toggleBold(): void;
  /** 斜体トグル（編集メニュー「斜体」・Ctrl+I と同じ動作）。実行後エディタへフォーカスを戻す。 */
  toggleItalic(): void;
  /** チェックボックストグル（編集メニュー・Ctrl+L と同じ動作）。実行後エディタへフォーカスを戻す。 */
  toggleCheckbox(): void;
  /** 破棄する。 */
  destroy(): void;
}

/**
 * Markdown の装飾記号（`#` 見出し・`>` 引用・`-`/`*` 箇条書き・`*`/`_`/`**` 強調・背鈎 `` ` ``・
 * リンク括弧 `[` `]` `(` `)` 等）に**色を付ける**ためのマーク専用タグ群（ユーザー要望）。
 *
 * @lezer/markdown は全マーク（HeaderMark/QuoteMark/ListMark/LinkMark/EmphasisMark/CodeMark）を一律
 * `tags.processingInstruction` に割り当てており、タグだけでは「見出しの # 」と「強調の * 」を区別できない。
 * そこで下の markdownMarkStyles（styleTags 拡張）でマーク種別ごとに専用タグへ付け替え、装飾対象
 * （見出し/引用/箇条書き…）に揃えた色を当てられるようにする。
 */
const mdMarkTag = {
  heading: Tag.define(),
  emphasis: Tag.define(),
  quote: Tag.define(),
  list: Tag.define(),
  link: Tag.define(),
  code: Tag.define(),
};

/**
 * マーク種別ごとに専用タグを付け直す Markdown パーサ拡張（既定の processingInstruction 一律割当を上書き）。
 * markdown() の `extensions` へ渡す。NodeSet.extend は同種 prop を後勝ちで上書きするため、
 * 基底の markdownHighlighting より後に適用されてマーク毎の色分けが効く。
 */
const markdownMarkStyles = {
  props: [
    styleTags({
      HeaderMark: mdMarkTag.heading,
      QuoteMark: mdMarkTag.quote,
      ListMark: mdMarkTag.list,
      LinkMark: mdMarkTag.link,
      EmphasisMark: mdMarkTag.emphasis,
      CodeMark: mdMarkTag.code,
    }),
  ],
};

/**
 * 構文ハイライト（リッチ版・**ユーザー要望で従来のモノトーン基調から色数・粒度を強化**）。
 * 色は**直書きせず** class を割り当て、色値は src/styles/app.css 側でトークン変数（--syntax-* 等）で当てる。
 * これにより html[data-theme] のライト/ダーク切替へ自動追従する（color 直書きだと追従できない）。
 *
 * 1 つの HighlightStyle を Markdown / HTML / 埋め込みコード（CSS・JS/TS）すべてに適用する
 *（syntaxHighlighting は現在の言語が産むタグへ無関係に当たる）。Markdown の見出し/強調/リンク等の
 * マークアップ系タグと、HTML/コードの tagName・attributeName・keyword・number 等を一通り網羅する。
 * `tags.function(...)` 等の修飾子付きタグは未マッチ時に基底タグの装飾へフォールバックする。
 */
const pikaHighlightStyle = HighlightStyle.define([
  // ── Markdown マークアップ ───────────────────────────────
  // 見出しは色＋太字で階層を示す。ソース表示の文字サイズは本文と同じ（修正2: cm-tok-h1/h2/h3 は
  // CSS 側で font-size:1em に戻す＝サイズで階層を出さない）。クラス割当はレベル識別のため残す。
  { tag: tags.heading1, class: "cm-tok-heading cm-tok-h1" },
  { tag: tags.heading2, class: "cm-tok-heading cm-tok-h2" },
  { tag: tags.heading3, class: "cm-tok-heading cm-tok-h3" },
  { tag: [tags.heading4, tags.heading5, tags.heading6, tags.heading], class: "cm-tok-heading" },
  // 太字/斜体/取り消し線は装飾＋僅かな色付け。
  { tag: tags.strong, class: "cm-tok-strong" },
  { tag: tags.emphasis, class: "cm-tok-emphasis" },
  { tag: tags.strikethrough, class: "cm-tok-strike" },
  // リンク/URL/リンクラベルは accent。
  { tag: [tags.link, tags.url, tags.labelName], class: "cm-tok-link" },
  // 引用（blockquote 本文）は落ち着いた色＋italic。
  { tag: tags.quote, class: "cm-tok-quote" },
  // 箇条書きマーカーは accent 系で僅かに立たせる。
  { tag: tags.list, class: "cm-tok-list" },
  // 水平線（`---`）。
  { tag: tags.contentSeparator, class: "cm-tok-hr" },
  // Markdown の装飾記号（マーク）に**色を付ける**（ユーザー要望）。種別ごとに装飾対象と揃えた色を当てる。
  // 見出し `#` は見出し色、引用 `>` は引用色、箇条書き `-`/`*` は accent、強調 `*`/`_` は専用色、
  // リンク括弧は accent、背鈎 `` ` `` はコード色。タグは markdownMarkStyles が付け直したもの。
  { tag: mdMarkTag.heading, class: "cm-tok-heading-mark" },
  { tag: mdMarkTag.quote, class: "cm-tok-quote-mark" },
  { tag: mdMarkTag.list, class: "cm-tok-list" },
  { tag: mdMarkTag.link, class: "cm-tok-link-mark" },
  { tag: mdMarkTag.emphasis, class: "cm-tok-emphasis-mark" },
  { tag: mdMarkTag.code, class: "cm-tok-code-mark" },
  // 上で個別化しなかった残りのマークアップ（HardBreak 等の processingInstruction・meta）は淡色。
  { tag: [tags.processingInstruction, tags.meta], class: "cm-tok-markup" },
  // インラインコード/等幅（言語指定の無いフェンス含む）= 等幅＋淡背景。
  { tag: tags.monospace, class: "cm-tok-monospace" },
  // エスケープ（`\*` 等）。
  { tag: tags.escape, class: "cm-tok-escape" },

  // ── コード共通（CSS・JS/TS・HTML 埋め込みスクリプト等） ──
  { tag: [tags.comment, tags.lineComment, tags.blockComment], class: "cm-tok-comment" },
  {
    tag: [
      tags.keyword,
      tags.controlKeyword,
      tags.operatorKeyword,
      tags.definitionKeyword,
      tags.moduleKeyword,
      tags.modifier,
      tags.self,
    ],
    class: "cm-tok-keyword",
  },
  { tag: tags.string, class: "cm-tok-string" },
  { tag: tags.regexp, class: "cm-tok-regexp" },
  { tag: [tags.number, tags.integer, tags.float], class: "cm-tok-number" },
  // 真偽値・atom・null・定数は数値と同系色（リテラル一群）。
  { tag: [tags.bool, tags.atom, tags.null, tags.constant(tags.variableName)], class: "cm-tok-bool" },
  // 型名・クラス名・名前空間。
  { tag: [tags.typeName, tags.className, tags.namespace], class: "cm-tok-type" },
  // プロパティ名（CSS プロパティ・オブジェクトキー）。
  { tag: tags.propertyName, class: "cm-tok-property" },
  // 関数名（定義・呼び出し）。
  { tag: [tags.function(tags.variableName), tags.function(tags.propertyName)], class: "cm-tok-function" },
  // 通常の変数名は地色のまま（虹色になりすぎないよう中立に保つ）。
  { tag: [tags.variableName, tags.definition(tags.variableName)], class: "cm-tok-variable" },

  // ── HTML/XML ───────────────────────────────────────────
  { tag: tags.tagName, class: "cm-tok-tag" },
  { tag: tags.attributeName, class: "cm-tok-attribute" },
  { tag: tags.attributeValue, class: "cm-tok-string" },
  { tag: tags.angleBracket, class: "cm-tok-bracket" },

  // ── 記号・演算子 ───────────────────────────────────────
  {
    tag: [
      tags.punctuation,
      tags.separator,
      tags.bracket,
      tags.squareBracket,
      tags.paren,
      tags.brace,
      tags.derefOperator,
    ],
    class: "cm-tok-bracket",
  },
  {
    tag: [
      tags.operator,
      tags.arithmeticOperator,
      tags.logicOperator,
      tags.compareOperator,
      tags.bitwiseOperator,
      tags.updateOperator,
      tags.definitionOperator,
      tags.typeOperator,
      tags.controlOperator,
    ],
    class: "cm-tok-operator",
  },
  // 構文エラー（不正トークン）は危険色で気づかせる。
  { tag: tags.invalid, class: "cm-tok-invalid" },
]);

/**
 * Markdown フェンスコード（```html / ```css / ```js …）の言語を解決する（codeLanguages・要件5.1）。
 * 同梱済みの lang-html/css/javascript のみを対象にする（追加依存はしない）。info 文字列は言語名のみを見る。
 * 該当なしは null（プレーン等幅のまま）。
 */
function fenceLanguage(info: string): Language | null {
  const tag = info.trim().toLowerCase().split(/[\s,{]/)[0];
  switch (tag) {
    case "html":
    case "htm":
    case "xhtml":
    case "xml":
    case "svg":
    case "vue":
      return htmlLanguage;
    case "css":
    case "scss":
    case "less":
      return cssLanguage;
    case "js":
    case "javascript":
    case "mjs":
    case "cjs":
    case "node":
      return javascriptLanguage;
    case "jsx":
      return jsxLanguage;
    case "ts":
    case "typescript":
      return typescriptLanguage;
    case "tsx":
      return tsxLanguage;
    default:
      return null;
  }
}

/**
 * ファイル種別に応じた言語拡張を選ぶ（要件5.1・md/HTML エディタ）。
 * - `.html`/`.htm`/`.xhtml`/`.svg`/`.xml` → HTML 言語（タグ/属性＋埋め込み `<style>`/`<script>` も色付け）。
 * - それ以外（既定）→ GitHub 風 Markdown（GFM: 取り消し線/表/タスクリスト）＋フェンスコードの言語色付け。
 * filePath 不明（タブ無しの空エディタ等）は Markdown を既定にする。
 */
function languageForPath(filePath: string | null | undefined): Extension {
  const lower = (filePath ?? "").toLowerCase();
  // 要望は「構文ハイライト強化」のみ。html()/markdown() が既定で同梱する**編集補助**
  //（autoCloseTags＝閉じタグ自動挿入・completeHTMLTags＝`<` 補完）は要件に無いので明示オフにする（足さない）。
  if (/\.(html?|xhtml|svg|xml)$/.test(lower)) return html({ autoCloseTags: false });
  // extensions=markdownMarkStyles で装飾記号（#/>/*/背鈎…）に種別ごとの色を付ける（ユーザー要望）。
  return markdown({
    base: markdownLanguage,
    codeLanguages: fenceLanguage,
    extensions: markdownMarkStyles,
    completeHTMLTags: false,
  });
}

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

/**
 * 単語構成文字（`\w` 相当＝ASCII 英数字＋`_`＋Unicode 文字/数字）。intraword `_` ガードに使う。
 * 日本語など語中の `_` 誤削除も防ぐため `\p{L}\p{N}` まで含める（空白・句読点は非単語＝強調の境界）。
 */
const WORD_CHAR = /[\p{L}\p{N}_]/u;

/**
 * 太字（`**`）/斜体（`_`）のトグル（自前コマンド・要件5.1）。Ctrl+B / Ctrl+I に割り当てる。
 *
 * `state.changeByRange` で全選択範囲を一括処理する（マルチカーソル/矩形選択対応・手動オフセット計算をしない）。
 * 各範囲について次の優先で判定する:
 *   (1) 選択の**内側**がマーカー対（先頭末尾が marker）なら外す＝アンラップ。
 *   (2) 選択の**外側**がマーカー対（直前直後が marker）なら外す＝アンラップ。
 *   (3) いずれでもなければ marker で包む（空選択は marker 対の中央へカーソルを置く）。
 * `userEvent:"input.wrap"` を付けてラップ操作を 1 つの Undo 境界にまとめる（往復で元へ戻せる）。
 * ドキュメント文字以外は触らない（「データを失わない」死守）。
 *
 * **intraword `_` ガード（要件5.1・データを失わない）**: `_`（斜体）のときだけ、CommonMark の語中強調
 * 無効規則に倣い、外側マーカーが両側とも単語構成文字に挟まれる語中 `_`（例: `foo_bar_baz` の `bar` を
 * 選んで Ctrl+I）ではアンラップせず (3) ラップへ倒す。識別子のアンダースコアを誤って削除しない。
 * `**`（太字）は語中強調が有効なのでこのガードは適用しない（従来どおり）。
 * 既知制限: `**a** **b**` 全選択時の内側アンラップ崩れは今回は直さない。
 */
function toggleWrap(marker: string): Command {
  const mlen = marker.length;
  // `_` のときだけ intraword ガードを効かせる（`**` は語中強調が有効なので無効）。
  const guardUnderscore = marker === "_";
  return (view: EditorView): boolean => {
    const { state } = view;
    // 絶対位置の 1 文字を返す（範囲外は ""＝非単語扱い＝強調の境界）。
    const charAt = (pos: number): string =>
      pos >= 0 && pos < state.doc.length ? state.sliceDoc(pos, pos + 1) : "";
    // 外側マーカーの前後（beforePos/afterPos）が両方とも単語構成文字＝語中 `_` か。
    const isIntraword = (beforePos: number, afterPos: number): boolean =>
      WORD_CHAR.test(charAt(beforePos)) && WORD_CHAR.test(charAt(afterPos));
    const spec = state.changeByRange((range) => {
      const { from, to } = range;
      const selected = state.sliceDoc(from, to);
      // (1) 選択の内側がマーカー対 → 内側の marker を 2 つ削除してアンラップ。
      // `_` の語中ガード: 外側マーカー（選択先頭/末尾の `_`）の直前 from-1・直後 to が両方単語文字なら
      // 語中 `_` とみなしアンラップを見送る（(3) ラップへ倒す）。
      const innerPair =
        to - from >= mlen * 2 && selected.startsWith(marker) && selected.endsWith(marker);
      if (innerPair && !(guardUnderscore && isIntraword(from - 1, to))) {
        const innerTo = to - mlen;
        return {
          changes: [
            { from, to: from + mlen, insert: "" },
            { from: innerTo, to, insert: "" },
          ],
          // marker 対を除いた内側がそのまま選択として残る（先頭は動かず末尾が 2*mlen 縮む）。
          range: EditorSelection.range(from, to - mlen * 2),
        };
      }
      // (2) 選択の外側がマーカー対 → 外側の marker を 2 つ削除してアンラップ。
      const before = state.sliceDoc(Math.max(0, from - mlen), from);
      const after = state.sliceDoc(to, Math.min(state.doc.length, to + mlen));
      // `_` の語中ガード: 外側マーカー（[from-mlen,from] と [to,to+mlen]）の直前 from-mlen-1・直後 to+mlen が
      // 両方単語文字なら語中 `_`（例: foo_bar_baz の bar）とみなしアンラップせず (3) ラップへ倒す。
      const outerPair = before === marker && after === marker;
      if (outerPair && !(guardUnderscore && isIntraword(from - mlen - 1, to + mlen))) {
        return {
          changes: [
            { from: from - mlen, to: from, insert: "" },
            { from: to, to: to + mlen, insert: "" },
          ],
          // 直前 marker が消えるぶん選択全体が mlen ぶん手前へずれる。
          range: EditorSelection.range(from - mlen, to - mlen),
        };
      }
      // (3) それ以外 → marker で包む。空選択は marker 対の中央へカーソルを置く。
      return {
        changes: [
          { from, insert: marker },
          { from: to, insert: marker },
        ],
        range:
          from === to
            ? EditorSelection.cursor(from + mlen)
            : EditorSelection.range(from + mlen, to + mlen),
      };
    });
    view.dispatch(state.update(spec, { scrollIntoView: true, userEvent: "input.wrap" }));
    return true;
  };
}

/** 太字トグル（Ctrl+B・編集メニュー）。keymap とメニュー双方から再利用する。 */
const toggleBoldCmd = toggleWrap("**");
/** 斜体トグル（Ctrl+I・編集メニュー）。keymap とメニュー双方から再利用する。 */
const toggleItalicCmd = toggleWrap("_");

/** 行頭のタスク記法（`- [ ]` / `* [x]` / `1. [X]` 等）を捉える正規表現。グループ1=記号〜`[`、2=チェック文字、3=`]`。 */
const TASK_LINE_RE = /^(\s*(?:[-*+]|\d+\.)\s+\[)([ xX])(\])/;

/**
 * チェックボックスのトグル（自前コマンド・要件5.1）。Ctrl+L に割り当てる。
 * 選択が跨る各行のうち**タスク記法行のみ**を対象に、チェック文字を ` `↔`x` で切り替える。
 * 素の行をタスク化はしない（記法行限定）。複数行・複数選択に対応し、同じ行は 1 回だけ処理する。
 * チェック文字 1 文字だけを置換し、文書の他の文字は壊さない（「データを失わない」）。
 */
const toggleCheckbox: Command = (view: EditorView): boolean => {
  const { state } = view;
  // 選択が跨る行番号を重複なく集める（複数選択でも各行 1 回）。
  const lineNums = new Set<number>();
  for (const range of state.selection.ranges) {
    const startLine = state.doc.lineAt(range.from).number;
    let endLine = state.doc.lineAt(range.to).number;
    // 非空選択で終端がちょうど次行頭に乗る場合、その行は実質含まれていないので除外する
    //（off-by-one: 触っていない次行までトグル対象にしない＝「データを失わない」）。
    if (range.to > range.from && range.to === state.doc.line(endLine).from) endLine--;
    for (let n = startLine; n <= endLine; n++) lineNums.add(n);
  }
  const changes: { from: number; to: number; insert: string }[] = [];
  for (const n of lineNums) {
    const line = state.doc.line(n);
    const m = TASK_LINE_RE.exec(line.text);
    if (!m) continue;
    // チェック文字の絶対位置（記号〜`[` の長さ＝m[1].length 直後の 1 文字）。
    const checkPos = line.from + m[1].length;
    const next = m[2] === " " ? "x" : " ";
    changes.push({ from: checkPos, to: checkPos + 1, insert: next });
  }
  if (changes.length === 0) return false;
  view.dispatch(state.update({ changes, userEvent: "input.checkbox" }));
  return true;
};

/**
 * 特殊文字（全角スペース/NBSP/ゼロ幅）の可視化レンダラ（要件5.1・可視化のみ）。
 * 全角スペース U+3000 は □、その他（NBSP・ゼロ幅など）は · で見せる。**ドキュメント文字は不変**＝
 * これは replace widget の見た目だけで、コピー/保存される文字列は元のまま（正規化・変換は一切しない）。
 * title に U+xxxx を入れて何の文字かを示し（スクリーンリーダ向けに aria-label も）、class でテーマ追従させる。
 */
function renderSpecialChar(code: number): HTMLElement {
  const span = document.createElement("span");
  const isFullWidth = code === 0x3000;
  // 全角スペースは頻出（日本語字下げ）のため別 class でさらに控えめにできるようにする。
  span.className = isFullWidth ? "cm-special-char cm-special-fullwidth" : "cm-special-char";
  span.textContent = isFullWidth ? "□" : "·";
  const label = "U+" + code.toString(16).toUpperCase().padStart(4, "0");
  span.title = label;
  span.setAttribute("aria-label", label);
  return span;
}

/**
 * 見出し（`#{1,6} `）単位のコード折りたたみを提供する foldService（要件5.1・折りたたみはキーボードのみ）。
 * 折りたたみコマンド（Ctrl+Shift+[）が対象行に対して問い合わせる。見出し行なら、その見出し行末から
 * 次の**同位以上の見出し**の直前（無ければ文末）までを畳む範囲として返す。折る中身が無ければ null。
 * fold gutter は配線しないため、この走査は明示折り操作時にだけ呼ばれる（常時走査しない＝軽量）。
 */
function headingFoldRange(
  state: EditorState,
  lineStart: number,
  lineEnd: number,
): { from: number; to: number } | null {
  const startLine = state.doc.lineAt(lineStart);
  const m = /^(#{1,6})\s/.exec(startLine.text);
  if (!m) return null;
  const level = m[1].length;
  const lastLineNo = state.doc.lines;
  let endLineNo = startLine.number;
  for (let n = startLine.number + 1; n <= lastLineNo; n++) {
    const line = state.doc.line(n);
    const hm = /^(#{1,6})\s/.exec(line.text);
    if (hm && hm[1].length <= level) break; // 次の同/上位見出しの手前で止める。
    endLineNo = n;
  }
  if (endLineNo === startLine.number) return null; // 見出し直下に中身が無い＝畳めない。
  // 見出し行末（lineEnd）から節末尾までを畳む。見出し行のテキスト自体は残す。
  return { from: lineEnd, to: state.doc.line(endLineNo).to };
}

/**
 * 重い装飾（長行/巨大ファイルで外す対象）を束ねる Compartment（設計の要点「heavyDecoCompartment」）。
 * backend の degrade（10万字/行で highlight_off・要件2.2/pika-core::huge）で `heavy=false` を渡すと
 * このコンパートメントを空へ reconfigure し、構文ハイライト/対応括弧/同一語強調/行末空白可視化を一括で外す。
 * これにより従来 baseExtensions に直挿しで巨大行でも常時走っていた syntaxHighlighting の穴（highlight_off
 * 未実効）も同時に塞ぐ。createEditor の初期化時に heavy に応じて of() するだけで、以後 reconfigure はしない。
 */
const heavyDecoCompartment = new Compartment();

/**
 * heavyDecoCompartment に載せる重い装飾の束（heavy=true のときだけ有効化）。
 * - syntaxHighlighting: 控えめな構文ハイライト（baseExtensions からここへ移設）。
 * - bracketMatching: カーソル隣接の対応括弧を強調（CM6 標準・安価）。
 * - highlightSelectionMatches: 選択語と同一の語を薄く強調（.cm-selectionMatch・app.css で .cm-search-hit と弁別）。
 * - highlightTrailingWhitespace: 行末の空白を可視化（.cm-trailingSpace）。
 * - highlightSpecialChars: 全角スペース/NBSP の**可視化のみ**（□/·・装飾でドキュメント文字は不変）。
 *   ZWJ/ZWNJ は絵文字結合を壊すため可視化対象から外す（ZWSP/BOM は CM6 既定で可視化）。
 * - codeFolding + foldService: 見出し単位の折りたたみ（キーボードのみ・fold gutter は出さない）。
 * いずれも装飾のみでドキュメント文字は変えない（「データを失わない」死守）。長行（heavy=false）では一括で外れる。
 */
const heavyDecorations: Extension[] = [
  syntaxHighlighting(pikaHighlightStyle),
  bracketMatching(),
  highlightSelectionMatches(),
  highlightTrailingWhitespace(),
  // 全角スペース U+3000 と NBSP U+00A0 を既定の特殊文字集合へ追加して可視化する。render は見た目だけを
  // 差し替え、文字は置換しない（保存・コピーは元のまま＝データを失わない）。
  // ZWJ(U+200D)/ZWNJ(U+200C) は **可視化対象から外す**: ZWJ 連結絵文字を `·` で分解してしまうため
  //（絵文字結合保護）。ZWSP(U+200B)/BOM(U+FEFF) 等は CM6 既定で可視化される。
  highlightSpecialChars({
    addSpecialChars: /[\u00a0\u3000]/g,
    render: renderSpecialChar,
  }),
  // 見出し折りたたみ（codeFolding が fold 状態を保持し、foldService が畳む範囲を見出し走査で算出）。
  codeFolding(),
  foldService.of(headingFoldRange),
];

const baseExtensions: Extension[] = [
  lineNumbers(),
  highlightActiveLine(),
  history(),
  // 言語（markdown/html）は createEditor がファイル種別に応じて差し込む（languageForPath）。
  // 検索ハイライト（自前 StateField・要件5.4・U4）。空集合で開始（バー未起動時は何も描かない）。
  searchHighlightField,
  // マルチカーソルを有効化する（CM6 標準）。allowMultipleSelections で複数選択を許可し、
  // drawSelection で複数の選択/カーソルを描画する（既定の単一ネイティブ選択では複数カーソルが見えない）。
  // 矩形選択（Alt+ドラッグ）とその十字カーソル表示も併せて有効化する。マルチカーソルのキー
  // （Ctrl+Alt+↑↓）と Ctrl+D/Ctrl+Shift+L のコマンドは下の keymap が担う。
  EditorState.allowMultipleSelections.of(true),
  drawSelection(),
  rectangularSelection(),
  crosshairCursor(),
  // pika 自前のエディタ級コマンド束（要件5.1/5.4）。defaultKeymap より **前** に置き上書き優先する
  // （preventDefault でブラウザ既定も抑止）。アプリ級ショートカット（Ctrl+G/Ctrl+Tab 等）とは別系統で、
  // フォーカスがエディタにある間だけ効く（resolveShortcut には載せない＝design 方針）。
  // - Ctrl+B / Ctrl+I: 太字 `**` / 斜体 `_` のラップ/アンラップ。Ctrl+I は selectParentSyntax を上書きする。
  // - Ctrl+Shift+I: 退避した selectParentSyntax（構文単位で選択を広げる）。
  // - Ctrl+L: タスク記法行のチェックボックス（` `↔`x`）トグル。
  // - Ctrl+D / Ctrl+Shift+L: 次の同一語を選択（マルチカーソル追加）／同一語を全選択。
  keymap.of([
    { key: "Mod-b", run: toggleBoldCmd, preventDefault: true },
    { key: "Mod-i", run: toggleItalicCmd, preventDefault: true },
    { key: "Mod-Shift-i", run: selectParentSyntax, preventDefault: true },
    { key: "Mod-l", run: toggleCheckbox, preventDefault: true },
    { key: "Mod-d", run: selectNextOccurrence, preventDefault: true },
    { key: "Mod-Shift-l", run: selectSelectionMatches, preventDefault: true },
    // 折りたたみ（Ctrl+Shift+[ 畳む / Ctrl+Shift+] 開く / Ctrl+Alt+[ ] 全畳/全開）。
    // codeFolding 拡張（heavyDecorations 側）が無効な長行では何も起きない（degrade 時は素通し）。
    ...foldKeymap,
  ]),
  // Tab はタブ文字を挿入する（スペース展開しない・保守的既定＝要件5.2）。indentUnit を "\t" に固定し、
  // indentWithTab で Tab/Shift+Tab をインデント操作へ割り当てる。tab_width は**表示幅にのみ**効く値で、
  // ここで入るのは常にタブ文字 1 個（挿入スペース数ではない）。indentWithTab は defaultKeymap より
  // 先に置き、フォーカスがエディタにある間 Tab がインデントへ向かうようにする。
  indentUnit.of("\t"),
  keymap.of([indentWithTab, ...defaultKeymap, ...historyKeymap]),
];

/**
 * createEditor のオプション（旧 9 位置引数を options object 化・S7。意味・既定値は旧引数と同一）。
 */
export interface CreateEditorOptions {
  /** エディタを描画する DOM 要素。 */
  parent: HTMLElement;
  /** 初期内容。 */
  initialDoc: string;
  /** 編集（dirty 化）通知。保存ボタンの活性に使う。 */
  onChange: () => void;
  /**
   * カーソル/選択/内容の変化通知。右下ステータスの追従更新に使う（要件11.1）。
   * selectionSet（カーソル移動・選択）または docChanged（編集）のたびに発火する。
   */
  onCursorChange?: () => void;
  /**
   * 初期の折り返し ON/OFF（表示メニューのトグルが保持する現在値・既定 ON）。
   * 既定 ON で短文ファイルの不要な横スクロールバーを避ける（ui-design §120）。
   * タブ切替でエディタを作り直しても現在の折り返し設定を引き継ぐために初期値で渡す。
   */
  lineWrapping?: boolean;
  /**
   * タブの**表示幅**（EditorState.tabSize・settings.toml の tab_width・既定 4）。
   * 挿入文字（タブ文字）には影響せず、Tab 文字を画面上で何桁ぶんに見せるかだけを決める（要件5.2）。
   * lineWrapping と同じく、タブ切替でエディタを作り直しても現在値を初期値で引き継ぐ。
   */
  tabWidth?: number;
  /**
   * 重い装飾（構文ハイライト/対応括弧/同一語強調/行末空白可視化）を有効にするか（既定 ON）。
   * backend の degrade（highlight_off/editing_off＝10万字/行の巨大ファイル・要件2.2）が立つときは false を
   * 渡し、heavyDecoCompartment を空に初期化して重い装飾を外す（長行の編集応答 200ms 予算・固まらない）。
   */
  heavy?: boolean;
  /**
   * エディタのスクロール変化通知（エディタ→プレビュー片方向スクロール同期・S4・要件6.1 改訂）。
   * `.cm-scroller` の native scroll を passive で拾うだけで、ハンドラ側がスロットルと発火条件
   * （Markdown プレビュー可視かつ差分OFF）を判断する（このモジュールは判断を持たない）。未指定なら配線しない。
   */
  onScroll?: () => void;
  /**
   * 開いているファイルのパス（言語選択用・要件5.1）。拡張子で markdown / HTML を切替える
   * （languageForPath）。null/未指定（タブ無しの空エディタ等）は Markdown を既定にする。
   * 言語はタブ単位で固定のため Compartment は使わず生成時に確定する（タブ切替＝エディタ再生成）。
   */
  filePath?: string | null;
}

/** 指定ホスト要素に CM6 を生成する（options は CreateEditorOptions）。 */
export function createEditor(opts: CreateEditorOptions): EditorHandle {
  const {
    parent,
    initialDoc,
    onChange,
    onCursorChange,
    lineWrapping = true,
    tabWidth = 4,
    heavy = true,
    onScroll,
    filePath = null,
  } = opts;
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
        // 言語（markdown/html）をファイル種別で差し込む。heavy=false（巨大ファイルの段階制・要件2.2）では
        // 構文木の構築自体を省いてプレーン表示にする（重い HTML/markdown パースを回避＝固まらない）。
        heavy ? languageForPath(filePath) : [],
        // 重い装飾束を Compartment 経由で初期化する（heavy=false で空＝長行/巨大ファイルで外す）。
        heavyDecoCompartment.of(heavy ? heavyDecorations : []),
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
          // 選択は半透明の青（--selection-bg）。drawSelection の描画層（本文の背面）なので本文色は
          // --text-1 のまま透けて見える＝ダークでも白潰れしない。
          // CM6 デフォルト baseTheme はフォーカス時に高詳細度セレクタ
          // （&light.cm-focused > .cm-scroller > .cm-selectionLayer .cm-selectionBackground）で
          // 薄紫を当てるため、低詳細度のここでは詳細度負けする。!important で確実に上書きする
          // （これが無いと選択中＝フォーカス時だけ CM6 既定の薄紫、非フォーカス時のみ青になる）。
          "&.cm-focused .cm-selectionBackground, .cm-selectionBackground": {
            backgroundColor: "var(--selection-bg) !important",
          },
          // 本文のネイティブ ::selection は透明にして drawSelection の背面レイヤー一本に絞る。
          // ネイティブ選択を残すと半透明背景がグリフの前面に二重で重なり、文字が潰れて見える
          // （グローバル app.css の ::selection が .cm-content へ漏れるのも併せて無効化する）。
          ".cm-content ::selection, .cm-content::selection": {
            backgroundColor: "transparent",
          },
        }),
      ],
    }),
  });

  // エディタ→プレビュー片方向スクロール同期（S4・要件6.1 改訂）。`.cm-scroller` の native scroll を
  // passive で拾う（CM6 の updateListener は scroll を確実には拾わないため scrollDOM へ直接張る）。
  // 発火条件（Markdown プレビュー可視・差分OFF）とスロットルはハンドラ側（main.ts）が判断する。
  if (onScroll) view.scrollDOM.addEventListener("scroll", onScroll, { passive: true });

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
    toggleBold: () => {
      toggleBoldCmd(view);
      view.focus();
    },
    toggleItalic: () => {
      toggleItalicCmd(view);
      view.focus();
    },
    toggleCheckbox: () => {
      toggleCheckbox(view);
      view.focus();
    },
    destroy: () => {
      // スクロール同期リスナを外してから破棄する（view.destroy は DOM を外すが明示的に解除する）。
      if (onScroll) view.scrollDOM.removeEventListener("scroll", onScroll);
      view.destroy();
    },
  };
}
