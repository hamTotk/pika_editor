// 非テキストファイルの簡易ビューとエッジケース提示（要件12.2）。
// 判定の決定論部分は pika-core::nontext と同規則。ここは DOM 提示（画像簡易ビュー・「既定アプリで
// 開く」誘導・寸法プリチェックの結果反映）を担う（実描画は系統C で検証）。

/** 画像拡張子（要件12.2）。pika-core::nontext::classify_extension と同じ集合。 */
const IMAGE_EXTS = ["png", "jpg", "jpeg", "gif", "webp", "bmp", "ico"];

/** 画像の総ピクセル数上限（要件2.2/12.2＝6000万px）。pika-core::nontext::MAX_IMAGE_PIXELS と同値。 */
export const MAX_IMAGE_PIXELS = 60_000_000;

/** 拡張子（ドット付き/なし可）が画像か。 */
export function isImageExt(name: string): boolean {
  const dot = name.lastIndexOf(".");
  if (dot < 0) return false;
  return IMAGE_EXTS.includes(name.slice(dot + 1).toLowerCase());
}

/**
 * 既知テキスト拡張子（要件12.2）。**pika-core::nontext::classify_extension の TEXT_EXTS の写し。要 同期**。
 * 上流（Rust）が正典。ここを増減したら必ず Rust 側 TEXT_EXTS と一致させる（isImageExt が IMAGE_EXTS を
 * 複製しているのと同じ作法＝判定の二重化を許す代わりに同期責務を明示する）。
 */
const TEXT_EXTS = [
  "md", "markdown", "html", "htm", "txt", "json", "jsonl", "csv", "tsv", "xml",
  "yaml", "yml", "toml", "rs", "ts", "js", "tsx", "jsx", "css", "py", "c",
  "cpp", "h", "hpp", "go", "java", "sh", "log",
];

/**
 * ファイル名からファイル種別を分類する（要件12.2）。
 * **pika-core::nontext::classify_file_name の写し。要 同期**（3ルールと IMAGE_EXTS/TEXT_EXTS を Rust と一致させる）。
 * image=画像簡易ビュー、text=CM6 でテキスト編集、unsupported=非対応バイナリ（既定アプリで開く誘導）。
 * 1. 拡張子なし（ドットを含まない＝Dockerfile/Makefile/README/LICENSE）→ text（CM6 で開ける・回帰修正）。
 * 2. dotfile（先頭 `.` で以降にドット無し＝.gitignore/.editorconfig/.env）→ text。
 * 3. それ以外は最後のドット以降を小文字化した拡張子で判定（IMAGE_EXTS/TEXT_EXTS、未知は unsupported）。
 * 取りこぼし（許容）: `.env.local` は dotfile 規則に当たらず拡張子 `local` が未知のため unsupported。
 * 関数名は呼び出し互換のため classifyExtension のままだが、判定はファイル名全体を見る（上流 Rust が正典）。
 */
export function classifyExtension(name: string): "text" | "image" | "unsupported" {
  // 1. 拡張子なし（ドットを一切含まない）はテキストへ寄せる。
  if (!name.includes(".")) return "text";
  // 2. dotfile（先頭 `.` で、それ以降にドットが無い）はテキストへ寄せる。
  if (name.startsWith(".") && !name.slice(1).includes(".")) return "text";
  // 3. それ以外は最後のドット以降の拡張子（小文字化）で従来判定。
  const ext = name.slice(name.lastIndexOf(".") + 1).toLowerCase();
  if (IMAGE_EXTS.includes(ext)) return "image";
  if (TEXT_EXTS.includes(ext)) return "text";
  return "unsupported";
}

/** 画像簡易ビューの表示モード（要件12.2「ウィンドウフィットと等倍の切替程度」）。 */
export type ImageFit = "fit" | "actual";

/**
 * 画像簡易ビューを描画する（表示のみ・編集/変換なし＝要件12.2）。
 * 寸法プリチェックはバックエンド（custom protocol or command）が行い、上限超は呼び出し側が
 * showOpenExternally へ振り分ける。ここは上限内の画像のみ受ける。
 *
 * onSetFit を渡すと上端に「ウィンドウフィット/等倍」の小さなトグルツールバーを出す（要件12.2
 * 「ウィンドウフィットと等倍の切替程度」）。省略時はツールバー無し（既存呼び出しとの後方互換）。
 * 現在 fit のボタンに aria-pressed=true ＋ .on を付け、クリックで onSetFit(f) を呼ぶ（再描画は呼び出し側）。
 */
export function renderImageView(
  host: HTMLElement,
  src: string,
  fit: ImageFit = "fit",
  onSetFit?: (f: ImageFit) => void,
): void {
  host.replaceChildren();
  host.hidden = false;
  if (onSetFit) {
    const bar = document.createElement("div");
    bar.className = "image-toolbar";
    const addBtn = (label: string, value: ImageFit): void => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "image-fit-btn";
      btn.textContent = label;
      const on = fit === value;
      btn.classList.toggle("on", on);
      btn.setAttribute("aria-pressed", String(on));
      btn.addEventListener("click", () => onSetFit(value));
      bar.appendChild(btn);
    };
    addBtn("ウィンドウフィット", "fit");
    addBtn("等倍", "actual");
    host.appendChild(bar);
  }
  const img = document.createElement("img");
  img.src = src;
  img.alt = "画像プレビュー";
  img.className = fit === "fit" ? "image-fit" : "image-actual";
  host.appendChild(img);
}

/**
 * 「既定のアプリで開く」誘導を描画する（要件12.2）。
 * - 巨大画像（寸法プリチェックで上限超）・非対応バイナリの両方で使う。
 * - 行き止まりにせず次の一手（既定アプリで開くボタン）を提示する（要件12 縮退時の next-action）。
 */
export function renderOpenExternally(
  host: HTMLElement,
  message: string,
  onOpenExternally: () => void,
): void {
  host.replaceChildren();
  host.hidden = false;
  const box = document.createElement("div");
  box.className = "open-externally";
  const text = document.createElement("p");
  text.textContent = message;
  box.appendChild(text);
  const btn = document.createElement("button");
  btn.type = "button";
  btn.textContent = "既定のアプリで開く";
  btn.addEventListener("click", onOpenExternally);
  box.appendChild(btn);
  host.appendChild(box);
}
