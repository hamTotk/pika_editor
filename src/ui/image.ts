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

/** 画像簡易ビューの表示モード（要件12.2「ウィンドウフィットと等倍の切替程度」）。 */
export type ImageFit = "fit" | "actual";

/**
 * 画像簡易ビューを描画する（表示のみ・編集/変換なし＝要件12.2）。
 * 寸法プリチェックはバックエンド（custom protocol or command）が行い、上限超は呼び出し側が
 * showOpenExternally へ振り分ける。ここは上限内の画像のみ受ける。
 */
export function renderImageView(host: HTMLElement, src: string, fit: ImageFit = "fit"): void {
  host.replaceChildren();
  host.hidden = false;
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
