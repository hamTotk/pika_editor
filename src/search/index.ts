// 検索/置換バーの UI＋状態機械（U4/U5・要件5.4）。
//
// 自前 StateField+Decoration（editor/index.ts）と組で動く。`@codemirror/search` は使わない
// （CM6 既定検索 keymap と競合させない）。backend の search_in_text/replace_in_text/replace_one を
// invoke し、全ヒットを薄く・現在ヒットを強くハイライトする（位置は backend が byte/UTF-16 の両方を返し、
// CM6 へは UTF-16 を渡す）。
//
// **第1段階のコスト注記（silent cap 回避）**: 打鍵ごとに getContent() の全文を invoke で backend へ
// 転送して検索する（最大で巨大ファイル閾値=数十MB のテキスト）。これは IPC 転送コストになるため
// debounce（~120ms）で打鍵連打を抑制する。完全な仮想化検索（差分転送・インクリメンタル）は後回し。
//
// **編集中の追従**: ハイライトは query 変更/置換時に再検索して更新する。手動タイプ中はハイライトが
// 古くなりうる（StateField は編集に map 追従するが新ヒットは拾わない）。再入力（または置換）で更新される。

import {
  searchInText,
  replaceInText,
  replaceOne,
  type SearchMatch,
  type SearchOptions,
} from "../ipc";
import type { EditorHandle } from "../editor";

/** 検索バーの公開 API（main から open/close する）。 */
export interface SearchController {
  /** バーを開く（find=検索のみ / replace=置換行も表示）。canSearch() が false なら開かない。 */
  open(mode: "find" | "replace"): void;
  /** バーを閉じてハイライトを消し、エディタへフォーカスを戻す。 */
  close(): void;
  /** いま開いているか。 */
  isOpen(): boolean;
}

/** createSearchController の依存（main が最新の editor/content を getter で渡す）。 */
export interface SearchDeps {
  /** バーを重ねる土台（#editor-pane）。position:relative 前提（app.css）。 */
  editorPane: HTMLElement;
  /** 現在のエディタ（タブ切替で作り直されるので毎回 getter で取る）。 */
  getEditor: () => EditorHandle | null;
  /** 検索対象の現在テキスト（null=エディタ無し/対象外）。 */
  getContent: () => string | null;
  /** このビューで検索できるか（差分ON/プレビュー/画像/第2段階は false）。 */
  canSearch: () => boolean;
  /** 件数/置換件数/無効正規表現などの通知（main の ui/notifications.notify を渡す）。 */
  notify?: (message: string, level: "info" | "warn" | "error") => void;
}

/** 検索バーの状態。 */
interface SearchState {
  open: boolean;
  mode: "find" | "replace";
  query: string;
  replacement: string;
  options: SearchOptions;
  matches: SearchMatch[];
  /** 現在ヒットの index（matches 空なら -1）。 */
  current: number;
  /** 件数上限で打ち切られたか（"27+" 表示）。 */
  truncated: boolean;
}

const DEBOUNCE_MS = 120;

export function createSearchController(deps: SearchDeps): SearchController {
  const state: SearchState = {
    open: false,
    mode: "find",
    query: "",
    replacement: "",
    options: { case_sensitive: false, whole_word: false, regex: false },
    matches: [],
    current: -1,
    truncated: false,
  };

  // ── DOM 構築（#editor-pane 右上に重ねるフローティングバー・色は app.css のトークン）──────────
  const bar = document.createElement("div");
  bar.className = "search-bar";
  bar.hidden = true;
  bar.setAttribute("role", "search");
  bar.setAttribute("aria-label", "検索と置換");

  // 検索行（常時表示）。
  const findRow = document.createElement("div");
  findRow.className = "search-row";

  const findInput = document.createElement("input");
  findInput.type = "text";
  findInput.className = "search-input";
  findInput.placeholder = "検索";
  findInput.setAttribute("aria-label", "検索文字列");

  const countLabel = document.createElement("span");
  countLabel.className = "search-count";
  countLabel.setAttribute("aria-live", "polite");
  countLabel.textContent = "";

  const prevBtn = mkButton("↑", "前のヒット");
  const nextBtn = mkButton("↓", "次のヒット");

  // オプショントグル（aria-pressed で現在値・クリックで再検索）。
  const caseBtn = mkToggle("Aa", "大文字小文字を区別");
  const wordBtn = mkToggle("\\b", "単語単位");
  const regexBtn = mkToggle(".*", "正規表現");

  const closeBtn = mkButton("×", "閉じる");
  closeBtn.classList.add("search-close");

  findRow.append(
    findInput,
    countLabel,
    prevBtn,
    nextBtn,
    caseBtn,
    wordBtn,
    regexBtn,
    closeBtn,
  );

  // 置換行（replace モードのみ表示）。
  const replaceRow = document.createElement("div");
  replaceRow.className = "search-row search-replace-row";

  const replaceInput = document.createElement("input");
  replaceInput.type = "text";
  replaceInput.className = "search-input";
  replaceInput.placeholder = "置換";
  replaceInput.setAttribute("aria-label", "置換文字列");

  const replaceOneBtn = mkButton("置換", "現在のヒットを置換");
  replaceOneBtn.classList.add("search-text-btn");
  const replaceAllBtn = mkButton("すべて置換", "すべて置換");
  replaceAllBtn.classList.add("search-text-btn");

  replaceRow.append(replaceInput, replaceOneBtn, replaceAllBtn);

  bar.append(findRow, replaceRow);

  // ── イベント結線 ─────────────────────────────────────────────────────────────
  // バー内のキーは window のショートカットディスパッチへ漏らさない（二重発火防止＝stopPropagation）。
  // keydown を捕まえて自前の Enter/Esc 等を処理しつつ、未処理キーも window へ流さない。
  bar.addEventListener("keydown", (e) => {
    e.stopPropagation();
  });

  findInput.addEventListener("input", () => {
    state.query = findInput.value;
    scheduleSearch();
  });
  findInput.addEventListener("keydown", (e) => onFindKey(e));

  replaceInput.addEventListener("input", () => {
    state.replacement = replaceInput.value;
  });
  replaceInput.addEventListener("keydown", (e) => onReplaceKey(e));

  prevBtn.addEventListener("click", () => step(-1));
  nextBtn.addEventListener("click", () => step(1));

  caseBtn.addEventListener("click", () => toggleOption("case_sensitive", caseBtn));
  wordBtn.addEventListener("click", () => toggleOption("whole_word", wordBtn));
  regexBtn.addEventListener("click", () => toggleOption("regex", regexBtn));

  closeBtn.addEventListener("click", () => close());

  replaceOneBtn.addEventListener("click", () => void doReplaceOne());
  replaceAllBtn.addEventListener("click", () => void doReplaceAll());

  // ── 検索（debounce → invoke）────────────────────────────────────────────────
  let debounceTimer: number | null = null;
  /** 検索結果の世代。await 中に新しい検索が始まったら古い結果を捨てる（後着で上書きしない）。 */
  let searchGen = 0;

  function scheduleSearch(): void {
    if (debounceTimer !== null) window.clearTimeout(debounceTimer);
    debounceTimer = window.setTimeout(() => {
      debounceTimer = null;
      void runSearch();
    }, DEBOUNCE_MS);
  }

  /**
   * 現在の query/options で全文検索し、結果でハイライト・件数表示・current を更新する。
   * keepCurrent=true なら可能な限り直前の現在ヒット位置を近傍維持する（オプション変更/置換後の再検索用）。
   */
  async function runSearch(keepCurrent = false): Promise<void> {
    if (!deps.canSearch()) return;
    const content = deps.getContent();
    if (content === null) return;
    if (state.query === "") {
      // 空クエリはハイライトを消し件数を空にする（検索していない状態）。
      state.matches = [];
      state.current = -1;
      state.truncated = false;
      deps.getEditor()?.clearSearch();
      renderCount();
      return;
    }
    const gen = ++searchGen;
    // 現在ヒットの開始バイトを覚えておき、再検索後に近傍維持に使う。
    const prevStart =
      keepCurrent && state.current >= 0 && state.current < state.matches.length
        ? state.matches[state.current].start
        : -1;
    try {
      const result = await searchInText(content, state.query, state.options);
      // 後着ガード: この await 中に新しい検索（query/option 変更）が走っていたら破棄する。
      if (gen !== searchGen) return;
      state.matches = result.matches;
      state.truncated = result.truncated;
      if (result.matches.length === 0) {
        state.current = -1;
      } else if (prevStart >= 0) {
        // 直前の現在ヒットに最も近い（start >= prevStart の最初、無ければ末尾）ヒットへ寄せる。
        const idx = result.matches.findIndex((m) => m.start >= prevStart);
        state.current = idx >= 0 ? idx : result.matches.length - 1;
      } else {
        state.current = 0;
      }
      applyHighlight(/* scroll */ !keepCurrent);
      renderCount();
    } catch {
      // 不正な正規表現は searchInText が reject する（regex モードのみ起こりうる）。
      // 件数を「無効な正規表現」にし、ハイライトを消す（throw を握って UI を保つ）。
      if (gen !== searchGen) return;
      state.matches = [];
      state.current = -1;
      state.truncated = false;
      deps.getEditor()?.clearSearch();
      countLabel.textContent = "無効な正規表現";
      countLabel.classList.add("search-count-error");
    }
  }

  /** 現在の matches/current をエディタへ反映する。scroll=true で現在ヒットを中央へ移動する。 */
  function applyHighlight(scroll: boolean): void {
    const editor = deps.getEditor();
    if (!editor) return;
    editor.setSearchMatches(
      state.matches.map((m) => ({ from: m.utf16_start, to: m.utf16_end })),
      state.current,
    );
    if (scroll && state.current >= 0) {
      const m = state.matches[state.current];
      editor.scrollToMatch(m.utf16_start, m.utf16_end);
    }
  }

  /** 件数表示を更新する（"3/27"・0 件は "0件"・打切りは "27+" で可視化）。 */
  function renderCount(): void {
    countLabel.classList.remove("search-count-error");
    if (state.query === "") {
      countLabel.textContent = "";
      return;
    }
    const total = state.matches.length;
    if (total === 0) {
      countLabel.textContent = "0件";
      return;
    }
    const totalText = state.truncated ? `${total}+` : `${total}`;
    countLabel.textContent = `${state.current + 1}/${totalText}`;
  }

  /** 次/前へ（dir=+1/-1）。末尾の次は先頭へラップ（巨大文書でも回り込む）。 */
  function step(dir: number): void {
    if (state.matches.length === 0) return;
    const n = state.matches.length;
    state.current = (state.current + dir + n) % n;
    applyHighlight(/* scroll */ true);
    renderCount();
  }

  function toggleOption(key: keyof SearchOptions, btn: HTMLButtonElement): void {
    state.options[key] = !state.options[key];
    btn.setAttribute("aria-pressed", String(state.options[key]));
    btn.classList.toggle("on", state.options[key]);
    // オプション変更は即再検索（debounce 不要・明示操作）。現在位置は近傍維持する。
    void runSearch(/* keepCurrent */ true);
  }

  // ── 置換（U5）────────────────────────────────────────────────────────────────
  /** 「すべて置換」: 全置換 → setContent（単一反映・dirty 化）→ 再検索 → 件数通知。 */
  async function doReplaceAll(): Promise<void> {
    if (!deps.canSearch() || state.query === "") return;
    const content = deps.getContent();
    if (content === null) return;
    try {
      const result = await replaceInText(
        content,
        state.query,
        state.replacement,
        state.options,
      );
      if (result.replaced === 0) {
        deps.notify?.("置換対象が見つかりませんでした", "info");
        return;
      }
      // 外部リロード注釈は付けない＝編集として反映し dirty 化する（保存対象になる）。
      deps.getEditor()?.setContent(result.text);
      // 置換後の新テキストで再検索してハイライト/件数を更新する。
      await runSearch();
      deps.notify?.(`${result.replaced} 件置換しました`, "info");
    } catch {
      deps.notify?.("無効な正規表現のため置換できませんでした", "warn");
    }
  }

  /** 「置換（1件）」: 現在ヒットの byte 位置を from に 1 件置換 → 再検索 → next 以降の次ヒットへ。 */
  async function doReplaceOne(): Promise<void> {
    if (!deps.canSearch() || state.query === "") return;
    const content = deps.getContent();
    if (content === null) return;
    // 現在ヒットの開始バイト。未選択（current<0）なら先頭(0)から最初のヒットを置換する。
    const from =
      state.current >= 0 && state.current < state.matches.length
        ? state.matches[state.current].start
        : 0;
    try {
      const result = await replaceOne(
        content,
        state.query,
        state.replacement,
        state.options,
        from,
      );
      if (!result.replaced) {
        // from 以降にヒットが無い＝先頭へラップして再検索（あれば current=0 へ寄る）。
        await runSearch();
        if (state.matches.length === 0) {
          deps.notify?.("置換対象が見つかりませんでした", "info");
        }
        return;
      }
      deps.getEditor()?.setContent(result.text);
      // 置換で位置がずれるので再検索し、result.next（新テキスト上の次検索開始バイト）以降の
      // 最初のヒットを現在ヒットにする（無ければ先頭へ）。
      await runSearchAndSelectFrom(result.next);
    } catch {
      deps.notify?.("無効な正規表現のため置換できませんでした", "warn");
    }
  }

  /**
   * 再検索し、start >= fromByte の最初のヒットを現在ヒットにして移動する（置換（1件）後の前進用）。
   * 該当が無ければ先頭ヒットへラップする。
   */
  async function runSearchAndSelectFrom(fromByte: number): Promise<void> {
    if (!deps.canSearch()) return;
    const content = deps.getContent();
    if (content === null) return;
    const gen = ++searchGen;
    try {
      const result = await searchInText(content, state.query, state.options);
      if (gen !== searchGen) return;
      state.matches = result.matches;
      state.truncated = result.truncated;
      if (result.matches.length === 0) {
        state.current = -1;
      } else {
        const idx = result.matches.findIndex((m) => m.start >= fromByte);
        state.current = idx >= 0 ? idx : 0;
      }
      applyHighlight(/* scroll */ true);
      renderCount();
    } catch {
      if (gen !== searchGen) return;
      state.matches = [];
      state.current = -1;
      deps.getEditor()?.clearSearch();
      countLabel.textContent = "無効な正規表現";
      countLabel.classList.add("search-count-error");
    }
  }

  // ── キー処理 ─────────────────────────────────────────────────────────────────
  function onFindKey(e: KeyboardEvent): void {
    if (e.key === "Enter") {
      e.preventDefault();
      // Shift+Enter=前 / Enter=次。
      step(e.shiftKey ? -1 : 1);
      return;
    }
    if (e.key === "Escape") {
      e.preventDefault();
      close();
      return;
    }
    // バー入力中に Ctrl+F/Ctrl+H が来たら再オープン扱い（入力を select して留まる）。
    if ((e.key === "f" || e.key === "h") && (e.ctrlKey || e.metaKey) && !e.altKey) {
      e.preventDefault();
      const wantReplace = e.key === "h";
      if (wantReplace) setMode("replace");
      findInput.select();
    }
  }

  function onReplaceKey(e: KeyboardEvent): void {
    if (e.key === "Enter") {
      e.preventDefault();
      void doReplaceOne();
      return;
    }
    if (e.key === "Escape") {
      e.preventDefault();
      close();
      return;
    }
    if ((e.key === "f" || e.key === "h") && (e.ctrlKey || e.metaKey) && !e.altKey) {
      e.preventDefault();
      if (e.key === "f") setMode("find");
      findInput.focus();
      findInput.select();
    }
  }

  /** モード（find/replace）を切り替え、置換行の表示を同期する。 */
  function setMode(mode: "find" | "replace"): void {
    state.mode = mode;
    replaceRow.hidden = mode !== "replace";
    bar.classList.toggle("search-bar-replace", mode === "replace");
  }

  // ── 公開 API ─────────────────────────────────────────────────────────────────
  function open(mode: "find" | "replace"): void {
    if (!deps.canSearch()) return; // このビューでは開かない（main 側で no-op 通知）。
    // 初回マウント（#editor-pane の先頭へ重ねる）。再オープンは付け替えない。
    if (!bar.isConnected) deps.editorPane.prepend(bar);
    bar.hidden = false;
    state.open = true;
    setMode(mode);
    // オプショントグルの表示を現在値へ同期する（再オープン時に状態を保つ）。
    syncToggle(caseBtn, state.options.case_sensitive);
    syncToggle(wordBtn, state.options.whole_word);
    syncToggle(regexBtn, state.options.regex);
    findInput.value = state.query;
    replaceInput.value = state.replacement;
    findInput.focus();
    findInput.select();
    // 既存クエリがあれば即ハイライトし直す（タブをまたいで再オープンしたとき等）。
    if (state.query !== "") void runSearch(/* keepCurrent */ true);
    else renderCount();
  }

  function close(): void {
    if (debounceTimer !== null) {
      window.clearTimeout(debounceTimer);
      debounceTimer = null;
    }
    // 進行中の検索結果が後から反映されないよう世代を進める。
    searchGen++;
    state.open = false;
    bar.hidden = true;
    deps.getEditor()?.clearSearch();
    // Esc/閉じるでエディタへフォーカスを戻す（キーボード操作の連続性を保つ）。
    deps.getEditor()?.focusEditor();
  }

  function isOpen(): boolean {
    return state.open;
  }

  return { open, close, isOpen };
}

// ── DOM ヘルパ（class はトークン CSS が当てる・色は直書きしない）────────────────────
function mkButton(label: string, ariaLabel: string): HTMLButtonElement {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "search-btn";
  btn.textContent = label;
  btn.title = ariaLabel;
  btn.setAttribute("aria-label", ariaLabel);
  return btn;
}

function mkToggle(label: string, ariaLabel: string): HTMLButtonElement {
  const btn = mkButton(label, ariaLabel);
  btn.classList.add("search-toggle");
  btn.setAttribute("aria-pressed", "false");
  return btn;
}

function syncToggle(btn: HTMLButtonElement, on: boolean): void {
  btn.setAttribute("aria-pressed", String(on));
  btn.classList.toggle("on", on);
}
