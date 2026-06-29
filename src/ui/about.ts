// 「バージョン情報」画面（要件13「About にバージョン・同梱 OSS ライセンスを表示」）。
//
// 旧実装は notify("pika 0.1") のトーストのみで、要件13 の「About に OSS ライセンスを同梱」を満たして
// いなかった。本モーダルはバージョン（exe 単一源）と同梱第三者 OSS のライセンス全文（exe 埋め込みの
// THIRD_PARTY_NOTICES）をスクロール表示する。
//
// modal.ts の openModal は「どのボタンも押すと閉じて値を resolve」する土台で、スクロール領域や
// 「閉じない」ボタン（ログフォルダを開く）を持つ About には合わない。よって overlay/box・Tab トラップ・
// Esc・modalDepth 共有は同 CSS クラス＋enterModal/leaveModal を流用しつつ、本モジュールで直接組む。

import { aboutInfo, openLogFolder } from "../ipc";
import { enterModal, leaveModal } from "./modal";
import { notify } from "./notifications";

/**
 * バージョン情報モーダルを開く（メニュー「ヘルプ → バージョン情報」から）。
 * 情報取得（about_info）に失敗したら従来同様トーストで知らせて開かない。
 */
export async function showAboutModal(): Promise<void> {
  let version: string;
  let notices: string;
  try {
    const info = await aboutInfo();
    version = info.version;
    notices = info.notices;
  } catch {
    notify("バージョン情報を取得できませんでした", "error");
    return;
  }

  // 閉じた後にフォーカスを戻す元要素を退避（a11y・openModal と同じ作法）。
  const prevFocus = document.activeElement as HTMLElement | null;

  const overlay = document.createElement("div");
  overlay.className = "modal-overlay";
  const box = document.createElement("div");
  box.className = "modal-box about-box";
  box.setAttribute("role", "dialog");
  box.setAttribute("aria-modal", "true");
  box.setAttribute("aria-label", "pika バージョン情報");

  const header = document.createElement("div");
  header.className = "about-header";
  const title = document.createElement("span");
  title.className = "about-title";
  title.textContent = "pika";
  const ver = document.createElement("span");
  ver.className = "about-version";
  ver.textContent = `version ${version}`;
  header.append(title, ver);

  const desc = document.createElement("div");
  desc.className = "about-meta";
  desc.textContent = "Windows 向け超軽量 Markdown/HTML エディタ";
  const lic = document.createElement("div");
  lic.className = "about-meta";
  lic.textContent = "MIT License — Copyright (c) 2026 pika contributors";

  const licLabel = document.createElement("div");
  licLabel.className = "about-licenses-label";
  licLabel.textContent = "同梱サードパーティ OSS ライセンス:";

  const pre = document.createElement("pre");
  pre.className = "about-licenses";
  pre.textContent = notices;
  pre.tabIndex = 0; // キーボードでスクロールできるようフォーカス可能に。
  pre.setAttribute("aria-label", "同梱サードパーティ OSS ライセンス全文");

  const actions = document.createElement("div");
  actions.className = "modal-actions";
  const logBtn = document.createElement("button");
  logBtn.type = "button";
  logBtn.className = "modal-btn";
  logBtn.textContent = "ログフォルダを開く";
  const closeBtn = document.createElement("button");
  closeBtn.type = "button";
  closeBtn.className = "modal-btn primary";
  closeBtn.textContent = "閉じる";
  actions.append(logBtn, closeBtn);

  box.append(header, desc, lic, licLabel, pre, actions);
  overlay.appendChild(box);
  document.body.appendChild(overlay);

  // 表示中はグローバルショートカット（window keydown）を抑止する。
  enterModal();

  // Tab はライセンス領域→2 ボタンを循環（モーダル外へ抜けない）。初期フォーカスは「閉じる」。
  const focusables: HTMLElement[] = [pre, logBtn, closeBtn];
  closeBtn.focus();

  const close = (): void => {
    overlay.remove();
    document.removeEventListener("keydown", onKey, true);
    leaveModal();
    prevFocus?.focus?.();
  };

  const onKey = (e: KeyboardEvent): void => {
    // モーダル中のキーは背景（window ハンドラ）へ伝播させない。
    if (e.key === "Escape") {
      e.preventDefault();
      e.stopPropagation();
      close();
    } else if (e.key === "Tab") {
      e.preventDefault();
      const idx = focusables.indexOf(document.activeElement as HTMLElement);
      const dir = e.shiftKey ? -1 : 1;
      focusables[(idx + dir + focusables.length) % focusables.length]?.focus();
    }
    // Enter はフォーカス中ボタンのネイティブ click に委ねる（preventDefault しない）。
  };
  document.addEventListener("keydown", onKey, true);

  // 「ログフォルダを開く」は閉じない（要件12.3・既存 command 流用）。
  logBtn.addEventListener("click", () => {
    void openLogFolder().catch(() => notify("ログフォルダを開けませんでした", "error"));
  });
  closeBtn.addEventListener("click", close);
  // 背景（オーバーレイ自身）クリックで閉じる（box 内クリックは透過しない）。
  overlay.addEventListener("pointerdown", (e) => {
    if (e.target === overlay) close();
  });
}
