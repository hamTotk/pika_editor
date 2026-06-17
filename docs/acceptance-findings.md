# pika 実機テスト 所見ログ（系統C）

> `docs/acceptance.md`（系統C 手動チェックリスト）の実機検証中に見つけた不具合・気づきを記録する。
> acceptance.md は各項目の合否表、本書は「見つかった問題と対応」の追跡台帳。
>
> 重大度: 高（データ損失/クラッシュ/中心体験不能）／中（機能不全・規約違反）／低（軽微・体感）。
> 状態: 発見 → 修正済(未検証) → 検証済 ✅ ／ 見送り（要件改訂・別スプリント）。
>
> 検証環境: Release ビルド `build/x64-release/src/app/Release/pika.exe`、フィクスチャ
> `C:\Users\devuser\pika-accept`、外部変更ドライバ `pika-accept-tools/extchange.py`。

---

## F-001 メニュー・ツリー見出しの文字化け（UTF-8→CP932 誤変換）

- **重大度**: 中（UI 言語=日本語のみの規約に対し、メニュー/見出しが判読不能）
- **対応章**: 起動直後（G UI 構成全般）
- **現象**: メニューバー2番目が「網ャ網雑H網シ(R)」、ツリー列見出しが「網軸ぃ繞、網ォ」と化けて表示。
  「ファイル」「表示」「ヘルプ」「変更を監視中」等は正常。漢字＋半角カナ混在の典型的な
  「UTF-8 バイト列を CP932 として解釈」パターン。
- **根本原因**: 生の日本語ナロー文字列リテラルを wx API に直接渡し、`u8()`/`wxString::FromUTF8`
  を通していなかった（design 10章 K9「UI クラスに生文字列を直接書かない」・CLAUDE.md
  「Win32境界で UTF-16 に変換」の漏れ）。
  - `src/ui/main_frame.cpp:146` … `menu_bar->Append(review_menu, "レビュー(&R)")`
    （「レビュー」用 `MsgId` が未定義だった）
  - `src/ui/file_tree_panel.cpp:251` … `view_->AppendTextColumn("ファイル", ...)`
    （`MsgId::TreePaneTitle`="ファイル" が存在するのに未使用）
- **対応**: `MsgId::MenuReview`（="レビュー(&R)"）を追加。両所を `u8(MsgId::MenuReview)` /
  `message(MsgId::TreePaneTitle)`→`FromUTF8` 経由に変更。リビルド→再起動で目視確認。
- **状態**: 検証済 ✅（メニュー「レビュー(R)」・見出し「ファイル」が正常表示）

---

## F-002 プレビューが描画されない（真っ黒）／モード切替が反映されない

- **重大度**: 高（中心体験の半分＝プレビュー/差分が全く表示できない。E 章も WebView2 描画依存で全滅）
- **対応章**: B（WebView2 実描画）・E（差分）
- **現象**: ファイルを開きプレビュー/分割にすると、プレビュー面が真っ黒。モードを切替えても反映されない。
- **根本原因（複合）**: `src/ui/preview_view.cpp` のプレビューナビゲートが wxWebViewEdge の特性と噛み合って
  いなかった。
  1. `wxWebView::SetPage(html, "https://doc.pika/")` を使用していたが、`wxWebViewEdge::DoSetPage` は
     **baseUrl を無視**（`WXUNUSED`）し `NavigateToString` を使う。よって相対画像/リンクが解決できず
     （preview_builder.cpp:170 の「ページ URL=doc.pika からのナビゲート」前提が崩れる）、NAVIGATING の
     URL も doc.pika にならず `on_navigating` の内部判定が外れて **Veto→黒**・`nav_in_flight_` 固着で
     以後の更新が全保留＝「反映されない」。
  2. 代替に `LoadURL("https://doc.pika/...")` を使うと、ハンドラを `wxWebViewHandler("https")`（scheme名
     ="https"）で登録していたため、`wxWebViewEdge::LoadURL` のカスタムプロトコル・エミュレーション
     （webview_edge.cpp:1124-）が `https:` を剥がして `https://<host>/` を前置し **host を二重化**
     （`https://doc.pika///doc.pika/...`）→ やはり `on_navigating` 判定が外れ Veto→黒。
  3. ハンドラが `request.GetURI()` を `"https://host/"` 前提で使っていたが、wxWebViewEdge の `GetURI()` は
     **`name + ":" + path`（host を落とす）**。サブリソース（app.pika の CSS 等）は host 照合に失敗し
     無スタイル化していた（main doc は 1+2 の二重崩れが偶然相殺して通っていた）。
- **対応**（設計どおり「生成 HTML を doc.pika 予約パスから本体配信し doc.pika へ実ナビゲート」へ）:
  - `PreviewView` に `preview_html_`/`nav_gen_` を追加。`navigate()` は `SetPage` をやめ、生成 HTML を
    `DocPikaHandler` の予約パス `__pika_preview__.html` から配信し `https://doc.pika/__pika_preview__.html?g=N`
    へ `LoadURL`（`?g` はキャッシュ無効化・ハンドラ側で破棄）。
  - ハンドラの scheme 名を `"https"` → virtual host 名（`"doc.pika"`/`"app.pika"`）に変更（LoadURL 二重化回避）。
  - ハンドラの URI 取得を `GetURI()` → **`GetRawURI()`**（実 URL で host 照合）。
  - `on_navigating` は予約パス/初期ロードを許可、他トップレベル遷移のみ Veto して振り分け。
  - `on_preview_navigate`（MainFrame）は内部 doc.pika/app.pika/data: を誤起動しないようガード。
- **状態**: 検証済 ✅（分割で見出し/GFM表/タスクリスト/相対画像が描画。診断ログで
  `on_navigating preview_doc=1`→`served`→`on_loaded`→`img/sample.png` 配信を確認後、診断ログは撤去）

## F-003 プレビュー単独モードで固まる（分割は可）

- **重大度**: 中（プレビュー単独モードが使えない。中心体験の表示モード切替が片肺）
- **対応章**: B・表示モード（ui-design 8章）
- **現象**: 分割モードはプレビュー描画される一方、「プレビュー」単独に切替えるとソースが固まりプレビューも出ない。
- **根本原因**: `MainFrame::update_preview` のレイアウトが `wxSplitterWindow::Initialize(window)` で単一窓を
  差し替えていた。Initialize は構築直後の一回用で、単一ペイン間（ソースのみ⇔プレビューのみ）の差し替えに
  使うと再描画/サイズ更新が安定せず固まる。
- **対応**: 「まず `SplitVertically` で分割へ正規化 → 不要側を `Unsplit(window)` で畳む」方式へ変更
  （`Freeze/Thaw` でちらつき抑止）。これで全モード遷移が確実に出し分く。
- **状態**: 検証済 ✅（ソース/分割/プレビューの相互切替が破綻しない）

## F-004 Mermaid／KaTeX／コードハイライトが未実装（プレビューで生テキスト）

- **重大度**: 中（要件6.2/6.4・9.5 の必須機能が欠落。ただし素の Markdown プレビューは動作）
- **対応章**: B1（Mermaid/KaTeX 表示）
- **現象**: プレビューで Mermaid コードブロックと `$$…$$`/`$…$` が**生テキストのまま**表示され、図/数式に
  ならない（GFM 表・タスクリスト・相対画像は描画される）。
- **根本原因**: 設計（design.md 6章「該当記法がある時だけ `<script>` を出力・遅延読み込み・per-block の
  try/catch＋1秒タイムアウト＋失敗フォールバック＋件数通知」）に対し、**ベンダー JS/CSS（mermaid ~3MB /
  KaTeX ~2MB / highlight ~1MB）が `assets/` に未同梱**（`preview.css` のみ・`vendor.lock` も無し）で、
  `preview_builder`/`render` に**スクリプト注入が一切無い**。実装そのものが欠落している。
- **対応**: 未着手（小修正ではなく機能実装。ベンダー同梱・SHA-256 ピン・CSP 連携・per-block 描画・
  THIRD_PARTY_NOTICES が必要）。対応方針はユーザー判断待ち。
- **状態**: 未対応（要対応判断）

## 確認済み（自動・準備フェーズ）

| 項目 | 結果 | 根拠 |
|------|------|------|
| H5 CLI `--version`/`--help` | ✅ | exit 0・パイプ出力欠落/化けなし・制御がシェルへ戻る |
| J1 ワークスペース非汚染 | ✅ | フィクスチャ folder 内に pika 管理ファイルなし（起動・監視後も） |
| A9 WebView2 遅延起動 | ✅ | プレビュー未使用時、pika 由来の WebView2 ユーザーデータフォルダ・データルート未生成 |
