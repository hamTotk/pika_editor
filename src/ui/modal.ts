// テーマ準拠の自前モーダル（プロンプト/確認/三択）の共通土台（S7・旧 main.ts から集約）。
//
// **このモジュールは window.confirm / window.prompt を一切使わない。** この Tauri/WebView2 ビルドでは
// window.confirm/prompt がダイアログを出さず即値（confirm≒true）を返すため、確認/入力をネイティブに頼ると
// 黙ってデータを失う（記憶ノート pika-window-confirm-footgun / tauri-native-dialogs-unavailable）。
// よって 3種すべて自前 DOM モーダルで実装し、その共通骨格（overlay/box・prevFocus 退避復帰・modalDepth 増減・
// Tab フォーカストラップ・Esc・overlay 外側クリック・Enter のフォーカス中ボタン尊重）を openModal に一元化する。
//
// 旧 main.ts の promptText/confirmModal/confirmDiscardModal の挙動を **1ミリも変えない**:
// - 返り値型・既定ボタン・三択・初期フォーカスを保存。
// - Enter は document.activeElement（フォーカス中ボタン）を尊重し、フォーカス外/異常時はデータ保全側へ倒す。
// - IME 合成ガード（isComposing / keyCode 229 を素通し）は **モーダルごとの旧挙動を保存**する:
//   promptText / confirmDiscardModal は有効、confirmModal は旧来ガード無しのまま（imeGuard 既定 true / confirm は false）。

/**
 * 開いているモーダル数。>0 の間はグローバルショートカット（window keydown）を無効化し、入力/確認中の
 * 修飾キー操作（Ctrl+Shift+Enter＝確認済み / Ctrl+W＝タブを閉じる 等）が背景タブへ貫通するのを防ぐ。
 * 旧 main.ts のモジュールスコープ変数をここへ移し、main.ts は isModalOpen() で参照する。
 */
let modalDepth = 0;

/** いずれかの自前モーダルを表示中か（グローバルショートカット抑止の判定・旧 `modalDepth > 0`）。 */
export function isModalOpen(): boolean {
  return modalDepth > 0;
}

/**
 * openModal の土台に乗らないカスタムモーダル（About 等・スクロール領域や閉じないボタンを持つ）が
 * modalDepth を共有するための増減プリミティブ。openModal と同じく、表示中はグローバルショートカット
 * （window keydown）を無効化したい場合に開始/終了で呼ぶ。必ず対で呼ぶこと。
 */
export function enterModal(): void {
  modalDepth += 1;
}
export function leaveModal(): void {
  modalDepth = Math.max(0, modalDepth - 1);
}

/** モーダルボタンの見た目（class を分ける）。 */
type ModalButtonVariant = "primary" | "danger";

/** ボタン 1 個の定義（左→右の表示順で渡す）。 */
interface ModalButtonSpec {
  label: string;
  /** "primary"=既定アクション（青）/ "danger"=破壊的（赤）/ 省略=地味なボタン。 */
  variant?: ModalButtonVariant;
}

/** openModal の設定（T は resolve 値の型）。 */
interface ModalConfig<T> {
  /** メッセージ（`\n` は <br> で改行表示する。1 行ならそのまま 1 テキストノード＝textContent と等価）。 */
  message: string;
  /** テキスト入力欄を付けるか（promptText）。付けると input が focusables の先頭かつ初期フォーカス。 */
  withInput?: boolean;
  /** input の placeholder（withInput 時のみ）。 */
  inputPlaceholder?: string;
  /** ボタン定義（左→右）。 */
  buttons: ModalButtonSpec[];
  /** 初期フォーカス: "input"（withInput 時）またはボタン index。 */
  initialFocus: "input" | number;
  /** IME 合成ガードを掛けるか（既定 true）。confirmModal の旧挙動再現のため false 指定可。 */
  imeGuard?: boolean;
  /** Esc / overlay 外側クリック の解決値。 */
  cancelValue: T;
  /** ボタン i クリックの解決値（input 値も渡す＝promptText の OK が入力値を返すため）。 */
  buttonValue: (index: number, inputValue: string) => T;
  /** Enter の解決値（focusedIndex=フォーカス中ボタン index・該当なしは -1／input 値も渡す）。 */
  enterValue: (focusedIndex: number, inputValue: string) => T;
}

/** ボタンの variant を旧来の class 文字列へ写す（旧実装の class と一致）。 */
function buttonClass(variant?: ModalButtonVariant): string {
  if (variant === "primary") return "modal-btn primary";
  if (variant === "danger") return "modal-btn danger-btn";
  return "modal-btn";
}

/**
 * 自前モーダルの共通骨格（promptText/confirmModal/confirmDiscardModal の土台）。
 * 旧 3 実装の overlay/box・prevFocus・modalDepth・Tab トラップ・Esc・overlay 外側クリック・Enter 処理を一元化する。
 */
function openModal<T>(cfg: ModalConfig<T>): Promise<T> {
  return new Promise((resolve) => {
    // 閉じた後にフォーカスを戻す元要素を退避（右クリックしたツリー行など・a11y）。
    const prevFocus = document.activeElement as HTMLElement | null;
    const overlay = document.createElement("div");
    overlay.className = "modal-overlay";
    const box = document.createElement("div");
    box.className = "modal-box";
    box.setAttribute("role", "dialog");
    box.setAttribute("aria-modal", "true");
    const label = document.createElement("div");
    label.className = "modal-label";
    // 複数行メッセージ（\n）は <br> で改行表示する（1 行なら 1 テキストノード＝旧 textContent と等価）。
    cfg.message.split("\n").forEach((line, i) => {
      if (i > 0) label.appendChild(document.createElement("br"));
      label.appendChild(document.createTextNode(line));
    });

    let input: HTMLInputElement | null = null;
    if (cfg.withInput) {
      input = document.createElement("input");
      input.type = "text";
      input.className = "modal-input";
      input.placeholder = cfg.inputPlaceholder ?? "";
    }

    const actions = document.createElement("div");
    actions.className = "modal-actions";
    const buttonEls: HTMLButtonElement[] = cfg.buttons.map((spec) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = buttonClass(spec.variant);
      btn.textContent = spec.label;
      actions.appendChild(btn);
      return btn;
    });

    if (input) box.append(label, input, actions);
    else box.append(label, actions);
    overlay.appendChild(box);
    document.body.appendChild(overlay);

    // モーダル表示中はグローバルショートカット（window keydown）を無効化する。
    modalDepth += 1;

    // Tab フォーカスを box 内へ閉じ込める（input があれば先頭）。
    const focusables: HTMLElement[] = input ? [input, ...buttonEls] : [...buttonEls];
    // 初期フォーカス。
    if (cfg.initialFocus === "input" && input) input.focus();
    else if (typeof cfg.initialFocus === "number") buttonEls[cfg.initialFocus]?.focus();

    const imeGuard = cfg.imeGuard ?? true;

    const close = (value: T): void => {
      overlay.remove();
      document.removeEventListener("keydown", onKey, true);
      modalDepth = Math.max(0, modalDepth - 1);
      // フォーカスを開く前の要素へ戻す（キーボード操作位置を見失わない）。
      prevFocus?.focus?.();
      resolve(value);
    };

    const onKey = (e: KeyboardEvent): void => {
      // IME（日本語入力）変換確定の Enter で入力を誤確定しないよう、合成中（isComposing）/合成キー
      // （keyCode 229）は素通しして入力欄/ボタンに委ねる（旧挙動再現＝promptText/confirmDiscardModal のみ）。
      if (imeGuard && (e.isComposing || e.keyCode === 229)) return;
      // 処理可否に関わらず、モーダル中のキーは背景（window ハンドラ）へ伝播させない。
      if (e.key === "Escape") {
        e.preventDefault();
        e.stopPropagation();
        close(cfg.cancelValue);
      } else if (e.key === "Enter") {
        // Enter はフォーカス中のボタンを尊重する（focusedIndex で解決）。該当なし（input/異常）は -1。
        e.preventDefault();
        e.stopPropagation();
        const focusedIndex = buttonEls.indexOf(document.activeElement as HTMLButtonElement);
        close(cfg.enterValue(focusedIndex, input?.value ?? ""));
      } else if (e.key === "Tab") {
        // box 内の要素を循環させる（外へ抜けない）。
        e.preventDefault();
        const idx = focusables.indexOf(document.activeElement as HTMLElement);
        const dir = e.shiftKey ? -1 : 1;
        focusables[(idx + dir + focusables.length) % focusables.length]?.focus();
      }
    };
    document.addEventListener("keydown", onKey, true);

    buttonEls.forEach((btn, i) => {
      btn.addEventListener("click", () => close(cfg.buttonValue(i, input?.value ?? "")));
    });
    // 背景（オーバーレイ自身）クリックでキャンセル（box 内クリックは透過しない）。
    overlay.addEventListener("pointerdown", (e) => {
      if (e.target === overlay) close(cfg.cancelValue);
    });
  });
}

/**
 * テーマ準拠の自前プロンプトモーダル（名前入力）。Promise で OK=入力値 / Cancel=null を返す。
 * Enter は入力値を確定（フォーカス位置によらず）、Esc/外側/キャンセルは null。IME ガード有効。
 */
export function promptText(message: string, placeholder = ""): Promise<string | null> {
  return openModal<string | null>({
    message,
    withInput: true,
    inputPlaceholder: placeholder,
    buttons: [{ label: "キャンセル" }, { label: "OK", variant: "primary" }],
    initialFocus: "input",
    imeGuard: true,
    cancelValue: null,
    // OK(1)=入力値 / キャンセル(0)=null。
    buttonValue: (index, inputValue) => (index === 0 ? null : inputValue),
    // Enter は常に入力値を確定する（旧 close(input.value)）。
    enterValue: (_focusedIndex, inputValue) => inputValue,
  });
}

/**
 * テーマ準拠の自前確認モーダル（はい/いいえ）。Promise で OK=true / Cancel=false を返す。
 * 初期フォーカスは OK。Enter はフォーカス中ボタンを尊重し、キャンセル以外（OK/フォーカス外）は true へ倒す
 * （データ保全側＝確認を必ず通す）。**旧挙動どおり IME ガードは持たない**（imeGuard:false）。
 */
export function confirmModal(
  message: string,
  opts?: { okLabel?: string; danger?: boolean },
): Promise<boolean> {
  return openModal<boolean>({
    message,
    buttons: [
      { label: "キャンセル" },
      { label: opts?.okLabel ?? "OK", variant: opts?.danger ? "danger" : "primary" },
    ],
    initialFocus: 1, // OK
    imeGuard: false, // 旧 confirmModal はガード無し（挙動不変）。
    cancelValue: false,
    // キャンセル(0)=false / OK(1)=true。
    buttonValue: (index) => index !== 0,
    // Enter: キャンセル(0)以外（OK/フォーカス外 -1 含む）は true（旧 close(activeElement !== cancel)）。
    enterValue: (focusedIndex) => focusedIndex !== 0,
  });
}

/**
 * 保存前の外部変更（衝突）解決モーダル（修正4・最上位原則「データを失わない」）。
 *
 * ディスク上でファイルが外部変更されているのに保存しようとしたときの 4 択。Tauri/WebView2 では
 * window.confirm がダイアログを出さず即 true を返すため**必ず自前モーダル**（既存 footgun）。
 * 返り値:
 * - `"overwrite"`: 外部変更を退避してから上書き（backend の stash_incoming_before_overwrite が退避）。
 * - `"saveAs"`: 名前を付けて保存（外部変更を残す）。
 * - `"reload"`: ディスク内容で再読込し自分の編集を破棄（保存しない・danger）。
 * - `"cancel"`: 何もしない（Esc・外側クリック・キャンセル）。
 *
 * 初期フォーカスは既定の「退避して上書き」。Enter はフォーカス中ボタンを尊重し、フォーカス外/既定は
 * "overwrite"（退避してから上書き＝データを失わない側）へ倒す。IME ガード有効（破棄系を含むため）。
 */
export function conflictModal(
  message: string,
): Promise<"overwrite" | "saveAs" | "reload" | "cancel"> {
  return openModal<"overwrite" | "saveAs" | "reload" | "cancel">({
    message,
    // 左から キャンセル／再読込で破棄(danger)／別名で保存／退避して上書き(primary・既定を右端に)。
    buttons: [
      { label: "キャンセル" },
      { label: "再読込で破棄", variant: "danger" },
      { label: "別名で保存" },
      { label: "退避して上書き", variant: "primary" },
    ],
    initialFocus: 3, // 退避して上書き（データを失わない既定）
    imeGuard: true,
    cancelValue: "cancel",
    buttonValue: (index) =>
      index === 0 ? "cancel" : index === 1 ? "reload" : index === 2 ? "saveAs" : "overwrite",
    // Enter: キャンセル(0)/再読込(1)/別名(2)はそれを選び、退避して上書き(3)・フォーカス外(-1)は
    // "overwrite"（外部変更を退避してから上書き＝データを失わない側）。
    enterValue: (focusedIndex) =>
      focusedIndex === 0
        ? "cancel"
        : focusedIndex === 1
          ? "reload"
          : focusedIndex === 2
            ? "saveAs"
            : "overwrite",
  });
}

/**
 * テーマ準拠の自前三択モーダル（保存して切替／破棄して切替／キャンセル）。
 * 返り値は OK 系ボタンの選択（"save"|"discard"）／Esc・外側クリック・キャンセルは "cancel"。
 * 初期フォーカスは既定の「保存して切替」。Enter はフォーカス中ボタンを尊重し、保存/フォーカス外は "save"
 * （データを失わない側）へ倒す。IME ガード有効。
 */
export function confirmDiscardModal(
  message: string,
): Promise<"save" | "discard" | "cancel"> {
  return openModal<"save" | "discard" | "cancel">({
    message,
    // 左から キャンセル／破棄して切替／保存して切替（既定の保存を右端に＝primary 位置を揃える）。
    buttons: [
      { label: "キャンセル" },
      { label: "破棄して切替", variant: "danger" },
      { label: "保存して切替", variant: "primary" },
    ],
    initialFocus: 2, // 保存して切替
    imeGuard: true,
    cancelValue: "cancel",
    buttonValue: (index) => (index === 0 ? "cancel" : index === 1 ? "discard" : "save"),
    // Enter: キャンセル(0)/破棄(1)はそれを選び、保存(2)・フォーカス外(-1)は "save"（保全側）。
    enterValue: (focusedIndex) =>
      focusedIndex === 0 ? "cancel" : focusedIndex === 1 ? "discard" : "save",
  });
}
