# 引き継ぎ: window.confirm footgun の一掃 ＋ PR#4 系統C 実機検証

作成: 2026-06-27（PR #4 マージ直後・`main` = マージコミット 518aa18）。
このドキュメントは新セッションへの引き継ぎ。`main` から作業ブランチを切って進める。

## 背景（なぜ）

pika の Tauri/WebView2 ビルドでは `window.confirm` / `window.prompt` が**ダイアログを出さず即値を返す**
（`confirm` は常に `true`）。実機検証で判明済み（`src/main.ts` の `confirmModal` 新設コメント参照）。
このため `window.confirm` を使った確認ゲートは事実上「常に OK」で、**ユーザーに無断でデータを失う**。
最上位の設計原則「データを失わない」に直結する潜在バグ。確認/入力は必ず自前モーダル
（`confirmModal` = boolean 2択 / `promptText` = 文字列入力）を使う。

PR #4（テキスト編集機能群）のレビュー対応では `onSave` / `onSaveAs` の unmappable 確認 2 箇所のみ
`confirmModal` 化した。**以下が未対応で残っている。**

---

## タスク1: 残存 window.confirm を confirmModal 化（最優先・データ損失）

`src/main.ts` の **実呼び出し3箇所**（コメント行は対象外。`grep -n "window.confirm" src/main.ts` で
コメントと区別すること）:

1. **`closeTab`**（未保存タブを閉じる確認・`window.confirm("「…」に未保存の変更があります。保存せずに
   閉じると失われます。閉じますか？")`）
   → `await confirmModal(...)` へ。`closeTab` を `async` にするか、確認部分を切り出して await できる形にする。
   呼び出し側（Ctrl+W / ×クリック / 中クリック閉じ / `onCloseActiveTab`）の経路が壊れないこと。

2. **`reopenActiveWithEncoding`**（エンコーディング指定で開き直す前の破棄確認・
   `window.confirm("…で開き直すと、未保存の変更は破棄されます。続けますか？")`）
   → `await confirmModal(...)`（この関数は既に async）。

3. **`confirmDiscardUnsaved`**（フォルダ切替時・`window.confirm` を**2段**にして「保存／破棄／中止」の
   **三択**を表現している。1段目 OK=save、Cancel→2段目 OK=discard / Cancel=cancel）
   → `confirmModal` は2択なので**三択モーダルの新設が必要**。`"save" | "discard" | "cancel"` を返す
   自前モーダル（`confirmModal` / `promptText` と同じ作法・フォーカストラップ・Esc=cancel・
   IME ガード `isComposing`/`keyCode 229`）を作る。この関数の戻り型は既に `"save"|"discard"|"cancel"` なので
   呼び出し側（`onOpenFolder` 等の未保存確認）はそのまま使える。

### 注意・観点
- 各モーダルは既存 `confirmModal`（`src/main.ts`）/`promptText` の実装を踏襲（テーマ準拠・フォーカストラップ・
  `modalDepth` 増減でグローバルショートカット抑止・Esc/外側で cancel・IME 合成ガード）。
- `closeTab` を async 化する場合、連続で複数タブを閉じる経路や、閉じた後の隣タブ activate の順序が
  壊れないか確認。
- `onSaveAs` 内の `window.prompt(...)` フォールバック（保存ダイアログ不在の dev ブラウザ向け）は footgun では
  ないが、本番では到達しない経路。今回は触らなくてよい（必要なら promptText 化を検討）。
- 完了後、`pika-window-confirm-footgun` メモリ（`MEMORY.md` 索引）を「解消済み」へ更新する。

---

## タスク2: PR#4 系統C 実機検証（PrintWindow ハーネス）

ハーネス = `C:\Users\devuser\pika-accept-tools\shot.py`（PrintWindow `PW_RENDERFULLCONTENT` で WebView2 内容取得＋
SendInput キー/中ボタン/座標クリック注入。前面化必須・PowerShell deny 回避）。
debug 起動は **Vite 必須**（`npm run dev` → `cargo build -p pika-app --bin pika` → `cmd //c start "" target\debug\pika.exe <folder>`。
ビルド前に `taskkill //F //IM pika.exe`）。

未確認の実挙動（コードと自動ゲートは緑だが GUI 実機未確認）:
- **差分ON時の検索の件数表示**: 正規表現で `\s` / `\n` を検索したとき、改行のみのヒットが件数に
  数えられず（`DiffHandle.filterRenderable`）、件数と `.diff-search-hit` の見える数・Next/Prev が一致すること。
- **save-as の競合確認**: 別タブで未保存の `notes.md` を開いた状態で、別タブを `notes.md` へ save-as → 上書き
  確認モーダルが出て、キャンセルで保存中止・未保存が保持されること（データを失わない）。
- **全タブ一覧ドロップダウン**: タブを多数（30枚超）開いて一覧を出し、`max-height:60vh` + overflow で
  スクロールでき、キーボード ↓ で末尾タブへ到達できること。
- **preview-only で「名前を付けて保存」**（Codex P2 是正・da54f84）: `.md` をプレビューのみモードで開き、
  メニューの Save As が**有効**で機能すること（プレーン保存と同様）。
- **IME（日本語合成）**: SendInput で実 IME 合成（`keyCode 229`/`isComposing`）を再現できず PR#4 から
  手動確認繰り越し。変換確定 Enter で行へ移動/保存等が暴発しないこと、`promptText` で変換確定が
  モーダル確定にならないこと。
- **差分検索の TS 側 UTF-16 ミラー**（`computeSpansByLine`）でサロゲートペア（絵文字）境界がずれないこと。

---

## 進め方（このプロジェクトの規約）

- **実装は dev-generator サブエージェントへ委譲**し、メイン側は誘導/検証/コミット判断に専念
  （[[pika-impl-via-generator]]）。**generator 報告は鵜呑みにせず**メイン側で
  `cargo test` / `cargo build`（警告 deny）/ `npm run typecheck` を独立再実行して裏取り
  （[[review-findings-fix-complete]] の教訓）。
- substantial な実装は詳細計画承認後に eval-loop（閾値80）でも可（[[pika-prefers-eval-loop]]）。
- コミットメッセージは日本語・`type: 要約`・3行以内＋`Claude-Session` トレーラ（`.claude/docs/commit.md`）。
- 確認/入力ダイアログは `window.confirm`/`window.prompt` 禁止・自前モーダル必須
  （[[tauri-native-dialogs-unavailable]]）。
- 要件/設計に関わる変更は `docs/requirements.md` / `docs/design.md` の章を本文参照。

## 参照
- メモリ: `pika-window-confirm-footgun` / `tauri-native-dialogs-unavailable` / `pika-text-editing-features` /
  `pika-impl-via-generator` / `review-findings-fix-complete`
- PR #4: hamTotk/pika_editor #4（マージ済み・`main` 518aa18）
