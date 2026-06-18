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

## F-005 HTML プレビューが Markdown 経由で描画され、インライン `<style>` が消える／JS 検知通知が出ない

- **重大度**: 中（要件6.3/6.4 の「インライン CSS を完全レンダリング」「自己完結 HTML がブラウザ同等表示」が
  満たせない。B3 の JS 検知通知バーも欠落）
- **対応章**: B2（自己完結 HTML）・B3（スクリプト入り HTML）
- **現象**:
  - B2: `selfcontained.html`（`<head><style>` に罫線/影/カード）をプレビューすると、見出し・段落・表は
    出るが**罫線/影/カードが付かない**（インライン `<style>` が効いていない）。
  - B3: `withscript.html` をプレビューすると JS は実行されない（背景赤化・alert なし＝合格）が、
    「JavaScript を含む（無効化）／既定のブラウザで開く」**通知バーが出ない**。
- **根本原因（複合）**:
  1. `src/ui/main_frame.cpp` の `update_preview` ワーカーが**種別に関係なく** `render_markdown(source)` を
     呼んでいた。HTML 文書も Markdown として解釈され、`sanitize_html` 直送経路（design 6章）に乗らない。
  2. `src/core/render/html_sanitizer.cpp` の `forbidden_subtree_tags()` に `"style"` が入っており、
     **すべての `<style>` を中身ごと除去**していた。これは要件6.3/6.4「インライン CSS を完全レンダリング」・
     ヘッダ契約・design 6章「CSS `url()`/`@import` の遮断（=保持）」・CSP `style-src 'unsafe-inline'` と矛盾。
     正しくは style 属性と同じく「**危険 CSS（url()/@import/expression() 等）を含む `<style>` だけ丸ごと落とし、
     安全な `<style>` は中身ごと保持**」。
  3. `inspect_html`（JS 依存/外部リソース検知）が UI から**一度も呼ばれず**、`JsDetected`/`RemoteResource`
     通知が発火していなかった（モデル・種別文言・`wxLaunchDefaultBrowser` は既存）。通知バーもテキストのみで
     アクションボタン非対応だった。
- **対応**:
  - サニタイザ: `style` をサブツリー除去から外し、`<style>` 専用処理を追加（生テキスト CSS を `is_css_safe`
    で検査→安全なら `<style>…</style>` を原文保持、危険なら丸ごと除去）。既存の @import テストは維持し、
    安全 style 保持の回帰テストを追加。
  - `update_preview`: HTML 種別は `sanitize_html` 直送に分岐。あわせて `inspect_html` を実行し、
    `depends_on_js()` 時に `JsDetected` 通知（idempotent）を発火。
  - 通知バー: `JsDetected` 行に「既定のブラウザで開く」ボタンを追加（`row.tab_path` を `file:///` で起動）。
- **状態**: 検証済 ✅（B2: 罫線/影/カード/青見出し/明背景が描画。B3: JS 不実行のまま通知バー
  「文書に JavaScript が含まれています（プレビューで無効化）」＋「既定のブラウザで開く」ボタンが出現。
  gtest `KeepsSafeStyleBlock` 追加・x64-core-test PASS。B10 外部リソース opt-in は同じボタン基盤を
  流用して B 章後半で対応）

## F-006 欠落サブリソースで wx 既定ログのモーダル「Pika Error」が出る／相対リンクのタブ振り分け未実装

- **重大度**: 中（モーダルが操作をブロック＝「固まらない」に反する・内部絶対パスを露出。B7 機能欠落）
- **対応章**: B5（`../` 遮断）・B6（壊れ参照）・B7（リンク振り分け）
- **現象**:
  - B6: `broken-refs.md` をプレビューすると、欠落画像の読込失敗で**モーダルダイアログ**
    「Pika Error: can't open file 'C:\…\secret.png' (error 2)」が出る。壊れ画像自体はブラウザ標準
    アイコンで表示（pika プレースホルダではない）。
  - B5: `../../secret.png` は `normalize_to_absolute` がルートでクランプし `…\pika-accept\secret.png`
    へ丸めるため root 外には出ない（**セキュリティ境界は保持**）。意味は「拒否」でなく「クランプ＋
    not found」。
  - B7: 相対 `.md`/`.html` リンクをクリックしても**何も起きない**（`on_preview_navigate` が doc.pika を
    無視していた）。外部リンクは既定ブラウザで開く（OK）。
- **根本原因**:
  - F-006 本体: `DocPikaHandler`/`AppPikaHandler` の `wxFFile` 失敗時に wx 既定ログ先（`wxLogGui`）が
    モーダルを表示。欠落サブリソースは想定内の事象なのにエラーダイアログ化していた。
  - B7: `on_preview_navigate` が `https://doc.pika/...` を「後続で精緻化」として無視していた。
- **対応**:
  - 両ハンドラのファイル I/O を `wxLogNull` スコープで囲み、欠落時はモーダルを出さず
    `FinishWithError`（壊れ画像アイコンのみ）に倒す。
  - `on_preview_navigate` を実装：doc.pika 相対 URL をワークスペース根基準で絶対化し、
    `.md/.html/.txt` は `open_file`（既存タブはアクティブ化・重複を開かない）。存在しなければ
    新規空タブを作らず状態バー「リンク先が見つかりません」（`StatusLinkNotFound`）。それ以外の相対は
    既定ブラウザでローカルファイルを開く。
  - B5 のクランプ挙動は root 外開示が無いことを確認のうえ許容（要件「範囲外は拒否」を実害なく満たす）。
- **状態**: 検証済 ✅（B6: 欠落画像でモーダルが出ず壊れアイコンのみ。壊れリンクは空タブを作らず状態バー
  通知。B7: 相対 md/html→タブ（重複は活性化）・外部→ブラウザ・欠落→通知。x64-core-test PASS。
  B6 の pika プレースホルダ（data: URI）は design I5 の磨き込みとして別途判断）

## F-007 B章の未実装機能（スクロール同期 B8／プレビュー内検索 B9／外部リソース opt-in B10）

- **重大度**: 中〜低（B8/B9 は should・B10 は要件2.4/6.2 のオプトイン導線。既定オフ＝外部通信しない安全側は
  既に成立しているため、欠けているのは「許可する導線」のみ）
- **対応章**: B8・B9・B10（＋B11 はレイアウトのみ実装・差分ベースライン未供給）
- **現象/実装状況**:
  - B8 スクロール同期: `render` が `data-line` を注入せず、プレビュー側に同期 JS も無い＝**未実装**。
  - B9 プレビュー内検索: Ctrl+F の `wxWebView::Find` 結線が無い＝**未実装**。
  - B10 外部リソース opt-in: `inspect_html.has_external_resource` を UI が見ておらず、`RemoteResource`
    通知も opt-in（policy=Allowed への切替＋再ナビゲート）も無い＝**未実装**。既定 Blocked のため
    「開いただけで外部通信しない」安全側は成立。
  - B11 差分: 表示モード別レイアウト（ソース+差分／分割+差分／プレビュー+差分 grid）は実装済みだが、
    ベースライン（前回確認時点）が未供給（`old=空`・sprint6 結線待ち）で差分内容は全行追加表示になる。
- **対応**: 未着手（機能実装。F-004 と同様にユーザー判断で実装/見送りを決める）。
- **状態**: 未対応（要対応判断）

## F-008 差分ベースライン未結線で差分トグルが常時無効（B11 不能・E章の中心結線が欠落）

- **重大度**: 高（中心体験④「差分→確認済み」の表示側が動作しない。ただしデータ保存側＝確認スナップショットは
  正しく保存されている）
- **対応章**: B11・E（差分・確認済み・巻き戻し）
- **現象**: 差分トグル（Ctrl+Shift+D）が常に無効で ON にできない。未編集でもベースラインがあっても不可。
- **根本原因**: `evaluate_diff_toggle` は `has_baseline=false` で `NoBaseline` を返す（diff_mode_model.cpp:67）。
  `MainFrame::update_diff_toggle_state` が `ctx.has_baseline=false` を**ハードコード**（main_frame.cpp:930
  「ベースライン供給は sprint6 結線・現状は未取得」）。さらに `update_preview` の差分計算も
  `old=std::string_view{}`（空）で実ベースラインを読んでいない（main_frame.cpp:1038）。
  - 一方 `on_confirm` は `DocumentController::confirm` 経由で確認内容をスナップショットへ正しく保存している
    （ベースラインの**生成側は実装済み**。欠けているのは**読み出し→差分への供給**のみ）。
- **対応方針**: E 章作業として、(1) スナップショット index からアクティブ文書のベースライン有無を判定して
  `has_baseline` を立て、(2) 差分計算でベースライン content を `old` に供給する（ワーカー I/O・必要なら
  キャッシュ）。確認/巻き戻し（E章本体）と一体で結線する。
- **状態**: 検証済 ✅（段階2実装。`update_diff_toggle_state` の `has_baseline` を `has_diffable_baseline`
  （index の baseline_object 有無）で実値化＝差分トグルが確認済みベースラインありで有効化。`update_preview`
  worker で `restore_baseline(index, rel)` を旧側に供給し `engine.compute(old, current)` で累積差分を生成。
  確認/巻き戻し後に `update_preview` を呼び差分面を更新。実機で確認→外部変更→差分(±/単語強調/+-記号)を
  確認(E1/E2/E6 合格)。ctest 654 PASS。B11 はトグル有効化済みで再評価可。）

## F-009 C章の未実装/部分実装（C3 UTF-8保存選択肢／C2 エンコ開き直し／C4 新規／C6 検索置換／C9 200MB）

- **重大度**: 中（C3 はデータ救済の選択肢欠落・C6 は要件5.4 の検索置換・他は編集体験の基本機能）
- **対応章**: C2・C3・C4・C6・C9
- **実装状況**:
  - C3 表現不能文字: 保存中断＋警告（OKのみ）で**無確認の文字欠落は起きない（安全性◯）**が、受け入れ基準の
    「UTF-8で保存/確認/キャンセル」の**選択肢が無い**（救済導線なし＝on_save の BlockedEncoding 分岐が
    `wxOK` 警告のみ。main_frame.cpp:683）。
  - C2 エンコ指定で開き直し: エンコメニュー/再オープン UI が**無い**＝未実装。
  - C4 新規ファイル/タブ: 新規作成アクション（wxID_NEW 等）が**無い**＝未実装。
  - C6 正規表現＋$1 全置換: 検索置換 UI（core/search はあるが UI 結線なし）が**無い**＝未実装。
  - C9 200MB 第2段階の読み取り専用ビューア: 実機フィクスチャ困難・未確認。
- **対応**: C3 の「UTF-8で保存」選択肢を実装済み（dev-generator）。on_save の保存実体を `perform_save`
  ヘルパへ括り出し、通常保存と UTF-8 救済保存で同一の退避→アトミック書き込み経路を共有。BlockedEncoding
  時のみ `wxMessageDialog`（SetYesNoLabels「UTF-8で保存」「キャンセル」）を出し、Yes で UTF-8(BOMなし)
  再保存・doc_meta も UTF-8 へ更新。MsgId `NotifyBlockedEncodingChoice`/`SaveAsUtf8` 追加。
  ctest 647 PASS。C2/C4/C6 は機能実装でユーザー判断（後回し）。
- **状態**: C3 選択肢=検証済 ✅（実機で UTF-8/BOMなし/CRLF維持/絵文字保存を確認）。C2/C4/C6/C9 は未対応
  （要対応判断・後回し）。

## F-010 編集してもタブが未保存にならない（●記号なし・閉じ/終了確認が出ず編集消失リスク）／タブ一覧ボタン欠如

- **重大度**: 高（未保存編集を**無確認で閉じて失える**＝設計原則1「データを失わない」に反する。C10/C11 不能）
- **対応章**: C10（タブ状態記号）・C11（未保存の閉じ/終了確認）・C7（タブ一覧）
- **現象**:
  - 文書を編集してもタブに **●（未保存）が付かない**（C10）。
  - 未保存タブを Ctrl+W/×/終了しても**確認ダイアログが出ず黙って閉じる**（C11）。
  - タブ溢れ時に左右スクロールボタンは出るが**全タブ一覧ドロップダウンが無い**・隠れ未読バッジも無い（C7）。
- **根本原因**:
  - dirty 追跡の結線が**欠落**。`TabState.unsaved` フラグと `confirm_discard_unsaved`/`has_unsaved_tabs`
    （unsaved を見る）・タブ記号の畳み込みは実装済みだが、`set_unsaved(abs,true)` を**どこからも呼んで
    いない**（呼ぶのは保存時の false のみ）。`EditorPanel` は `is_dirty()`（SCI GetModify）を持つが、
    Scintilla の編集イベント（savepoint）を `MainFrame`→`TabManager` へ伝える結線が無い。
  - C7: `wxAuiNotebook` 生成スタイルに `wxAUI_NB_WINDOWLIST_BUTTON` が無い（main_frame.cpp:218-220）。
    隠れ未読バッジはカスタム `wxAuiTabArt` が要るため別件。
- **対応方針**: (1) `EditorPanel` に Scintilla の savepoint 通知（`wxEVT_STC_SAVEPOINTLEFT`/
  `SAVEPOINTREACHED`）を結線し dirty 変化コールバックを公開。`set_text_utf8` 後は `SetSavePoint`+
  `EmptyUndoBuffer` で初期クリーン。保存成功時に `SetSavePoint`（mark_clean）。(2) `MainFrame` が
  `open_file` でコールバックを `tabs_.set_unsaved(abs,dirty)`＋`refresh_tab_title` へ結線。(3) ノートブックに
  `wxAUI_NB_WINDOWLIST_BUTTON` を追加。隠れ未読バッジは後回し。
- **状態**: 検証済 ✅（dev-generator 実装。EditorPanel に savepoint 通知→dirty コールバック・保存後
  mark_clean、MainFrame open_file で結線。実機で C10 ●表示/保存解除/再編集再表示、C11 ×/Ctrl+W/終了の
  確認ダイアログ（保存/破棄/キャンセル・キャンセルで残る）、C7 全タブ一覧ドロップダウンを確認。ctest 647 PASS。
  隠れ未読バッジ（カスタム wxAuiTabArt）は後回し）

## F-011 自己保存抑制が未結線—pika 自身の保存が外部変更として未読化される（D8）

- **重大度**: 中（中心体験の未読＝外部/AI変更のはずが、自分の保存も未読/差分マーク化して誤誘導。
  データ損失ではない）
- **対応章**: D8（自己保存抑制）。C1 で「自分が保存した sjis-crlf.md が ◆/未読化」した観察の正体。
- **根本原因**: `WatcherCore::register_self_save(path, hash_lf, at)`（＋`SelfSaveGuard`・ハッシュ一致主条件）
  は実装済みだが、保存経路（`MainFrame::perform_save`/`on_save`）から**一度も呼ばれていない**。
  トークン未登録のため watcher は自分の書き込みを抑制できず外部変更として扱う。
- **対応方針**: `perform_save` の `write_atomic` 成功後に
  `watcher_->register_self_save(rel, core::watcher::content_hash_lf(abs).value(), GetTickCount64())`
  を呼ぶ。要点＝(1) path は監視ルート相対（`rel`。HashProbe が `root+"/"+rel`、FsEvent.path も rel）、
  (2) hash は HashProbe と同じ `content_hash_lf`（ディスク実バイトの LF 正規化ハッシュ。メモリ上の
  UTF-8 buffer ではなく書込済みファイルを読む）、(3) 時刻は watcher poll と同じ `::GetTickCount64()`
  クロック。UI スレッドで write 直後に同期登録＝後続の on_raw_event(CallAfter) より先に登録され順序安全。
- **状態**: 未対応（dev-generator で実装予定。D1〜D7 と合わせて実機検証）

## F-012 クリーンな開タブへの外部変更がライブリロードされない（D3・中心体験の核）

- **重大度**: 中〜高（pika の主目的＝AIの外部変更を確認する伴走。開いている文書が外部変更で更新されない
  のは中心体験「外部変更の反映」の欠落。データ損失ではない）
- **対応章**: D3（クリーンタブのライブリロード）。D1（ツリー未読バッジ）は動作。
- **現象**: `target.md` を開いた状態（未編集=クリーン）で外部から書き換えると、ツリーにバッジは付く（D1◯）
  が、**開いているエディタの内容が古いまま差し替わらない**（未保存にはならない）。
- **根本原因**: `MainFrame::apply_fs_events` は UnreadMarked でツリー未読化のみ行い、**開いているタブの
  Scintilla 内容を再読込する処理が無い**。`EditorPanel::set_text_utf8` のコメントにも「外部変更反映の
  単一 Undo は sprint4」とあり未着手だった。
- **対応方針**: (1) `EditorPanel` に `reload_text_utf8(utf8)` を追加＝`BeginUndoAction`/`EndUndoAction`
  で**単一 Undo** にまとめて全文置換し（`EmptyUndoBuffer` は呼ばない＝Ctrl+Z で旧内容に戻せる）、
  最後に `SetSavePoint()` でクリーン（ディスク一致）にする。(2) `apply_fs_events` の UnreadMarked で、
  対象が開タブ かつ **クリーン（`is_dirty()`==false）** のとき、`read_all`+`decode_auto` で再読込し
  `reload_text_utf8` で反映。`doc_meta_` の last_loaded_hash/encoding/has_bom も更新（次回保存の衝突基準
  を新内容へ）。アクティブタブならプレビューも更新。(3) **dirty タブは再読込しない**（編集を守る＝衝突は
  保存時の prepare_save が退避。D4）。未読バッジ（D1）は維持。
- **状態**: 未対応（dev-generator で実装予定。D2/D4〜D7 と合わせて実機検証）

## F-013 F5/resync がベースライン未確立で全ファイルを差分あり化（D6・F-008 と同根）

- **重大度**: 中（F5 でレビュー状態が壊れる＝全ファイル未読化。データ損失ではない）
- **対応章**: D6（F5 全体再スキャン）。B11/F-008（差分ベースライン）と同根。
- **現象**: F5 を押すと「再同期中」は一瞬（または見えず）・固まらないが、**ツリーの全ファイルに「差分あり」が
  付く**。
- **根本原因**: `open_workspace` が `WorkspaceController::set_baseline(...)` を呼ばず**ベースラインが空**
  （main_frame.cpp:249 で `WorkspaceController(workspace_)` 生成のみ・コメントに「ベースライン本体の供給
  は sprint6 結線」とある）。`on_resync_needed` の `resync(workspace_, workspace_ctl_.baseline())` が
  空ベースラインと突き合わせ、`resync` は baseline 不在のファイルを Created（新規）として全件イベント化
  → `apply_fs_events` が全件 UnreadMarked。
  - resync のプレスクリーンは「baseline の mtime+size と一致なら無変化（ハッシュも見ない）」なので、
    **開いた時点の (size,mtime,hash) でベースラインを seed すれば F5/ポーリングは正しく無変化判定する**。
- **対応方針**: `open_workspace` で現ファイル列挙から `BaselineMap`（rel→{size,mtime_ns,hash_lf}）を作り
  `set_baseline` する（＝起動時未読判定の基準＝design 9章）。ハッシュ全計算の起動コストが問題なら
  TaskRunner でバックグラウンド seed も検討。これは F-008（差分ビューのベースライン）・E章（確認/巻き戻し）
  と同じ「sprint6 ベースライン確立」であり、まとめて実装すると D6/B11/E が一気に解ける。
- **状態**: 検証済 ✅（段階1実装。`build_baseline_from_disk`＋`merge_index_into_baseline`（gtest 追加）で
  open 時にベースライン確立＝`MainFrame::establish_baseline`。未確認ファイルは現 size+mtime で seed しクリーン、
  確認済みは index の永続ベースラインで上書き→起動時 resync で確認後変更分のみ未読化。F5 は同 baseline を
  使うため無変更ならマーク無し。実機確認済み。ctest 654 PASS。差分内容/トグル（F-008/B11）は段階2で対応）

## F-014 アトミック保存の一時ファイルが watcher に漏れ「幽霊未読」を生む（E7・未読カウント不整合）

- **重大度**: 中（未読カウントとツリー表示が不整合になり、解消もできない＝レビュー状態の信頼を損なう。
  データ損失ではない・再起動で消える）
- **対応章**: E7（一括確認）で表面化。実体は watcher/未読の一時ファイル混入。
- **現象**: 4ファイルを外部変更→「すべて確認済みにする」で未読が 5→1 に減るが、残り1件に対応する
  **ツリーの差分マークが無く、どのファイルか分からない**。index.json は全 entry `unread=False`（confirm-all は
  対象を確認済みにできている）＝**メモリ上の未読集合と永続が不整合**。
- **根本原因**: `util::write_atomic` の一時ファイルは `make_temp_path` で `<最終名>.pika-<pid>-<tick>.tmp`
  としてワークスペース内に作られる（同一ボリューム rename のため）。これが watcher の除外対象でないため、
  保存/確認/巻き戻し（＝頻発するアトミック書き込み）のたびに Created イベントを生む。対応する削除/rename
  イベントが取りこぼし/合成で消えると、未読集合に **存在しない `.tmp` パスが残る**＝ツリーにノードが無い
  幽霊未読。`on_confirm_all` は対象を `read_all`（disk.is_err→continue）するため、消えた `.tmp` は
  targets に入らず確認できず未読のまま残る。
- **対応方針**: pika 自身のアトミック書き込み一時ファイル（`.pika-<...>.tmp` パターン）を watcher から
  不可視にする。`is_pika_temp_file(rel)` ヘルパを追加し、(1) ライブイベント正規化（`make_raw_event`）で
  該当を drop、(2) `resync` の列挙でも除外。ユーザーの `.tmp` を巻き込まないよう `.pika-` 中置＋`.tmp` 末尾
  の厳密パターンで判定。既存の幽霊は再起動（新ビルド）で消える。
- **修正**: `is_pika_temp_file(rel)` を `src/core/watcher/resync.{h,cpp}` に追加（`.pika-` 中置かつ `.tmp`
  末尾の厳密判定）。(1) `make_raw_event`（`watch_event_map.cpp`）で該当ライブイベントを drop、(2) `resync`
  の `enumerate` で該当を列挙対象から除外。テスト追加（`resync_test.cpp` / `watch_event_map_test.cpp`）。
- **状態**: ✅ 修正済み・実機検証済み（E7 再試行で 3ファイル外部変更→「すべて確認済みにする」で未読 0、
  幽霊未読残らず。gtest 全 PASS）

## F-015 単一「確認済みにする」に確定直前の再照合が無い（E5・見ていない内容のベースライン化）

- **重大度**: 中（レビューの信頼性に関わる。差分描画後〜確認クリックの間に外部変更が入ると、ユーザーが
  見ていない内容を黙ってベースライン化する。データ損失ではない・発生にはレースが必要・ライブリロードが
  通常はプレビューを同期させるが、契約上は守られていない）
- **対応章**: E5（確認済み確定直前の再照合）。design 5.4「差分ビュー経由の場合は差分計算時のディスク
  スナップショットを採用し、確定直前に mtime/ハッシュを再照合して不一致なら中断・再差分（E2。ユーザーが
  見ていない内容をベースライン化しない）」。
- **現象/根本原因**: `on_confirm`（`src/ui/main_frame.cpp`）は確認時に `util::read_all(abs)` で
  ディスクを**新規読込し、ユーザーが差分で見た内容と照合せず無条件にベースライン化**する。confirm_all は
  `freeze_hash`（開始時点ハッシュ）で並行変化を検知・スキップするが（`review_flow.cpp` 136行）、単一確認
  には同等のガードが無い。コメント（904-905行）は「確定直前に再読込…再照合の素材」と意図を示すが、実際の
  照合ロジックが未実装。
- **対応方針**: 差分/プレビュー描画時に「ユーザーが見た内容」の参照ハッシュ（＝差分の新側 `source` の
  LF正規化 XXH3）をファイル単位で保持する。`on_confirm` で確定直前にディスクを再読込し、ハッシュを参照と
  照合：(a) 一致→その内容でベースライン化（従来どおり安全）、(b) 不一致→ベースライン化を中断し、新ディスク
  内容で差分を再描画＋通知「外部変更を検出しました。差分を更新したので内容を確認してから再度確認済みに
  してください」。参照が無い（一度もプレビューしていない）場合は従来挙動にフォールバック。
- **修正**: core 側で再照合（gtest 可能に）。`ReviewTarget` に `expected_hash`（見た内容＝差分新側
  ＝エディタ内容の LF正規化 XXH3 hex）を追加し、`ReviewFlow::confirm` は確定直前に現ディスク内容
  （`target.content`）のハッシュと照合、不一致なら `ErrorCode::Cancelled` で中断（ベースライン未更新）。
  UI（`on_confirm`）は (a) 未保存なら `NotifyConfirmNeedsSave` で保存を促し中断、(b) エディタ内容の
  ハッシュを `expected_hash` に設定、(c) `Cancelled` を受けたら `reload_open_tab_if_clean`＋`update_preview`
  で再差分し `NotifyConfirmStaleRediff` を通知（未読維持）。空 `expected_hash`（タブ未オープン等）は
  従来どおり再照合せず更新（後方互換）。テスト3件追加（一致→更新／不一致→Cancelled・未変更・未読維持／
  空→後方互換）。
- **状態**: ✅ 修正済み・検証済み（GUI: ①未保存ガードのダイアログ表示 ②外部変更→確認で差分消去・誤通知
  なしの回帰を実機確認。不一致→中断経路は gtest 3件で担保。ctest x64-core-test 全 PASS=662件）

## 確認済み（自動・準備フェーズ）

| 項目 | 結果 | 根拠 |
|------|------|------|
| H5 CLI `--version`/`--help` | ✅ | exit 0・パイプ出力欠落/化けなし・制御がシェルへ戻る |
| J1 ワークスペース非汚染 | ✅ | フィクスチャ folder 内に pika 管理ファイルなし（起動・監視後も） |
| A9 WebView2 遅延起動 | ✅ | プレビュー未使用時、pika 由来の WebView2 ユーザーデータフォルダ・データルート未生成 |
