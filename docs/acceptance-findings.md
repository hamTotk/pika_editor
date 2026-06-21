# pika 実機テスト 所見ログ（系統C）

> `docs/acceptance.md`（系統C 手動チェックリスト）の実機検証中に見つけた不具合・気づきを記録する。
> acceptance.md は各項目の合否表、本書は「見つかった問題と対応」の追跡台帳。
>
> 重大度: 高（データ損失/クラッシュ/中心体験不能）／中（機能不全・規約違反）／低（軽微・体感）。
> 状態: 発見 → 修正済(未検証) → 検証済 ✅ ／ 見送り（要件改訂・別スプリント）。
>
> 検証環境: Release ビルド `build/x64-release/src/app/Release/pika.exe`、フィクスチャ
> `C:\Users\nonpr\pika-accept`、外部変更ドライバ `pika-accept-tools/extchange.py`。

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

## F-004 Mermaid／KaTeX／コードハイライト（実装・実機検証済み）

- **重大度**: 中（要件6.2/6.4・9.5 の必須機能が欠落。ただし素の Markdown プレビューは動作）
- **対応章**: B1（Mermaid/KaTeX 表示）
- **現象**: プレビューで Mermaid コードブロックと `$$…$$`/`$…$` が**生テキストのまま**表示され、図/数式に
  ならない（GFM 表・タスクリスト・相対画像は描画される）。
- **根本原因**: 設計（design.md 6章「該当記法がある時だけ `<script>` を出力・遅延読み込み・per-block の
  try/catch＋1秒タイムアウト＋失敗フォールバック＋件数通知」）に対し、**ベンダー JS/CSS（mermaid ~3MB /
  KaTeX ~2MB / highlight ~1MB）が `assets/` に未同梱**（`preview.css` のみ・`vendor.lock` も無し）で、
  `preview_builder`/`render` に**スクリプト注入が一切無い**。実装そのものが欠落している。
- **対応**: 実装済み（generator）。
  - **ベンダー同梱** `assets/vendor/`：KaTeX 0.16.11（`katex.min.js`/`katex.min.css`/
    `katex-auto-render.min.js`＋`fonts/KaTeX_*.woff2` 20 種）・Mermaid 10.9.1（`mermaid.min.js`）・
    highlight.js 11.9.0（`highlight.min.js`＝common ビルド＋`hljs-github.min.css`/
    `hljs-github-dark.min.css`）。合計増分 **約 3.84 MiB（4,029,735 bytes）**（うち mermaid 3.18MiB が主因）。
    `assets/vendor.lock`（SHA-256＋取得元＋バージョン）・`assets/THIRD_PARTY_NOTICES`
    （MIT/MIT/BSD-3＋フォント OFL 1.1 全文）を新設。CMake は既存 `copy_directory assets/` で
    サブディレクトリごと build 出力へ複写（追加変更なし）。
  - **条件付き注入**：`core/render/preview_features.{h,cpp}` の `detect_preview_features()` が
    Markdown ソースを走査して mermaid/math/code の有無を判定（コードフェンス内の `$` は数式と誤検出
    しない）。`controller/preview_builder` の `build_head`/`build_feature_scripts` が該当時のみ
    vendor の `<link>`/`<script>` を出す（未使用時は一切出さない＝原則③）。すべて app.pika 配信。
  - **per-block 堅牢化**：`assets/preview-bootstrap.js`（pika 自作）が各ブロックを try/catch＋
    1 秒タイムアウト（Promise.race）で囲み、失敗時は元のコードブロック表示へ戻してエラーバッジ。
    失敗件数を `window.pika.postMessage` でネイティブへ通知 →`PreviewView::on_script_message`
    →`MainFrame::update_render_failed_notification` →通知バー（新 `NotificationKind::RenderFailed`）。
  - **CSP**：既存テンプレートのまま動作（緩和なし）。Mermaid 10.9.1 は `securityLevel:'strict'`＋
    `globalThis` 環境で `Function` コンストラクタ分岐に入らず `unsafe-eval` 不要（バンドルに `eval(`/
    `new Function`/動的 `import()` 無し）。KaTeX の inline style は `style-src 'unsafe-inline'`、
    フォントは `font-src https://app.pika`、ハイライト/数式 CSS は `style-src https://app.pika` で通る。
  - **テスト**：gtest 40 件（`PreviewFeaturesTest` 16・`PreviewBuilderTest` 24〔うち新規 11〕）。
    注入の出し分け・サニタイズ相互作用（ユーザー由来 script 除去・注入 vendor 保持）・全 script が
    app.pika ホスト・差分 grid の左プレビューにも注入・未使用時無注入を検証。x64-release ビルド成立・
    x64-core-test 723 件 PASS / 0 FAIL。
- **実機検証中の追加修正（2件）**:
  1. **通貨ロバスト化**：`line_has_math`（preview_features.cpp）が「価格は $5 と $9」のような同一行2つの
     通貨 `$` を数式と誤検出していた。定番ルール「`$` の直後が数字なら数式開始としない」を**検出側**
     （line_has_math）と**描画側**（preview-bootstrap.js の KaTeX auto-render 前処理：`$`直後数字を
     一時マスク→復元）の両方に適用。回帰 gtest 6 件追加。
  2. **サニタイザ二重エスケープ修正（真の Mermaid 描画バグ）**：`html_sanitizer.cpp::append_escaped_text`
     が md4c の1段エスケープ済み Text（`--&gt;`）を再エスケープし `--&amp;gt;`（2段）にしていた。
     ブラウザ textContent は1段しか復号せず `--&gt;` が mermaid に渡り "Syntax error"。escaper を
     **実体認識化**（`valid_entity_at`：妥当な `&name;`/`&#DDD;`/`&#xHHH;` は素通し・素の `&` のみ
     `&amp;`）。`<`/`>` は無条件エスケープ・**属性値エスケープ（append_escaped_attr_value）は別関数で
     不変**＝XSS 境界維持。16種の攻撃ペイロードで敵対監査合格。
- **状態**: ✅ 実装・実機検証済み。図(SVG)/数式(ブロック・インライン)/ハイライト(cpp/python)の実描画・
  通貨非崩れ・壊れた Mermaid のみ失敗フォールバック＋通知1件 を系統C実機で確認。gtest 735 件 PASS。
  残：mermaid 3.3MB の初回ロードコスト（図を含む文書のみ・条件付き注入で未使用時はゼロ）＝既知の許容コスト。

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

## F-016 F章（状態復元・設定・テーマ・ジャンプリスト）の GUI 結線が未実装（sprint7 GUI 配線の欠落）

- **重大度**: 中〜大（要件10章の主要機能が GUI で動かない。ただし core/controller は実装＆gtest 済みで、
  欠落は「系統B/C の配線」に限定。データ損失には直結しない）
- **対応章**: F1〜F6。実機 GUI 検証で初めて表面化（系統A の gtest は通っているため見落とされていた）。
- **現象（コードで確認）**:
  - **F1 完全復元（要件10.1・sprint7 must）**: `core/state/state_io`（state.json 読み書き）と
    `controller/restore_plan`（RestorePlan 組み立て）は実装・gtest 済みだが GUI から呼ばれない。
    `on_close_window`（main_frame.cpp 1566行）は state.json を**保存しない**。`app_controller` は
    `restore_previous`（引数なし起動）を算出するが（app_controller.cpp 95行）、`main_gui.cpp` で
    **消費されず**、`plan.folder` 指定時の `open_workspace` しか実行しない（main_gui.cpp 260行）。
    →フォルダ/タブ/カーソル/表示モード/差分トグル/ツリー展開/ペイン/ウィンドウ位置の復元が動かない。
  - **F2 消失タブの安全遷移（要件10.1）**: 復元（F1）に依存する部分が未配線（タブの「削除済み」表示
    自体は存在）。
  - **F3/F4 settings.toml の反映/破損時（要件10.3/10.4）**: `core/settings` は実装済みだが
    `main_gui.cpp` は `default_settings()` で起動し（225-226行・「settings.toml の実配置・監視配線は
    sprint7」コメント明記）、**settings.toml をディスクから読まず・監視もしない**。
  - **F5 ジャンプリスト（要件10.2・should）**: 該当コードがプロジェクトに**皆無**（JumpList /
    ICustomDestinationList 等の参照なし）。
  - **F6 OS テーマ追従（要件10.3/11.4）**: `on_sys_colour_changed`（main_frame.cpp 1462行）は
    `Refresh()` のみの骨格で「テーマトークンの再解決は sprint7」とコメント。実際の配色再適用は未実装。
- **対応方針（候補）**: (1) F1 — `on_close_window` で AppState を組み立て `state_io` で保存、起動時
  `restore_previous` なら `load_state`→`build_restore_plan`→MainFrame が機械的に消費して復元。
  (2) F3/F4 — `main_gui` でデータルートの settings.toml を読込（破損時は既定＋警告・直前有効値維持）、
  `core/watcher` で監視し変更を反映（読み取り専用＝書き戻さない）。(3) F6 — テーマトークンを
  `wxSysColourChangedEvent` で再解決し配色再適用。(4) F5 — Win32 ジャンプリスト（should・優先度低）。
- **状態（更新）**: ユーザー判断「must を実装（F1+F3/F4）」。
  - **F1 ✅ 実装・実機検証済み**: `on_close_window` で `save_session_state()`（AppState 組み立て→
    `state_io::save_state(<data_root>\state.json)`、失敗は握り潰しベストエフォート）。引数なし起動
    （`plan.restore_previous`）で `load_state`→`build_restore_plan`→`MainFrame::restore_session`。
    EditorPanel に caret/scroll の getter/setter、FileTreePanel に展開 get/set を追加。実機: フォルダ/
    タブ3つ/アクティブタブ/カーソル位置(offset218)/ツリー展開(sub)/ウィンドウ位置を復元確認。
    ctest 662 PASS。制限: 表示モードはウィンドウ単一(タブ毎でない)・preview_scroll 未対応・ペイン
    収納は機能未実装でN/A。**初回のグレースフル終了ハング観測は再現せず**（単一インスタンス転送で
    旧ハングプロセスへ転送した誤観測。以後 5回以上のグレースフル終了は全てクリーン＆state.json 保存）。
  - **F2 △ 部分対応**: restore は消失ファイルをスキップ（落ちない）・ワークスペース消失は空状態。
    「削除済み」タブとして開く挙動は未実装。
  - **F3/F4 ✅ 実装・実機検証済み**: コントローラ層（`settings_view::apply_settings`・`to_ui_settings`・
    `NotificationKind::SettingsError`）は実装済みだったので GUI 結線のみ追加。`MainFrame::load_and_watch_settings`
    （起動直後・タブを開く前に main_gui から1回）→`reload_settings_from_disk`（`<data_root>\settings.toml` を
    `read_all`→`load_settings(text, settings_)`→`apply_settings`：apply=true で settings_ 更新＋
    `reapply_settings_to_ui`〔全エディタへ make_editor_config→apply_config・ツリー除外は refresh_tree〕、
    parse_failed は settings_ 非更新＝直前値維持、warning/parse_failed は SettingsError 通知）。監視は
    wxTimer の poll（2秒・mtime+size プレスクリーン。`core::watcher::probe`）。MsgId 2件追加。実機: 起動中の
    wordWrap 編集→約2秒で long.md 折り返し変化(F3)／壊れTOML→警告＋直前値維持＋クラッシュなし(F4)。
    既知の軽微点: 初回ロード後に poll 基準値を seed しないため、起動時に既に警告/破損ファイルがある場合のみ
    初回ロードと初回pollで通知が二重化しうる（起動後編集の通常経路では発生しない・未修正）。
  - **F5/F6 後回し（ユーザー判断）**。F2 △ 部分対応のまま。

## F-017 削除済みファイルがツリーから消える（取り消し線で残らない・G9 未実装）

- **重大度**: 中（要件11.5/ui-design 5章の a11y「色非依存の削除表示」が効かない。ただしデータ損失なし
  ＝タブは [削除] 表示で残り、内容は退避(orphan)に保全）。
- **対応章**: G9（削除済みファイルの取り消し線）。実機: 開いていた g9-victim.md を外部削除→**ツリーから
  完全に消失**（取り消し線で残らない）。タブの [削除] 表示は正常。
- **根本原因**: `MainFrame::refresh_tree` は `enumerate_shallow_tree(workspace_, settings_)` で**ディスクを
  再列挙**し `build_view_model` で状態マークを付与する。削除ファイルはディスクに無い＝列挙に含まれない
  ため、`StateMark::Deleted`→取り消し線のレンダリング（`tree_view_model.cpp` 66-68・B2 で GetAttr 結線）
  は存在しても**削除ノードが生成されず発火しない**。`workspace_controller.cpp:89` も「外部削除＝ツリーから
  消える・タブ側は削除済み表示へ」と明記＝現実装は意図的にツリーから除去している。
- **対応方針（候補）**: ツリーが「既知の削除済みパス」（ベースラインを持つ確認済みファイルが消えた・または
  開いているタブのファイルが消えた）を保持し、ディスク列挙へ deleted=true ノードとしてマージする
  （サブディレクトリ階層への挿入が要る）。データ安全は満たされているため過剰実装は避ける（軽い・足さない）。
- **修正**: `WorkspaceController` に `deleted_` 集合（`Removed` で add・`Created`/`Modified`/rename 先で erase
  ＝再作成で通常表示へ復帰）＋accessor。controller 純粋関数 `merge_deleted_into_view_model(TreeRowVm&,
  deleted_rel_paths)`（ディスク列挙済みはスキップ＝二重防止・中間フォルダ作成・削除リーフは StateMark::Deleted）。
  `refresh_tree` で build_view_model 後にマージしてから set_root。テスト8件追加。
- **状態**: ✅ 修正済み・実機検証済み（外部削除した f017-victim.md がツリーに取り消し線で残る。ctest 666件 PASS）

## F-018 F6/Shift+F6 のペイン間フォーカス循環が未実装（G4）

- **重大度**: 低〜中（キーボードのみの中心体験は Tab/矢印/メニューアクセラレータ/Ctrl+Enter で完了可能。
  F6 循環が無いだけ＝代替手段あり。要件11.4/11.5・ui-design 13章・dev sprint8 should）。
- **対応章**: G4。実機: キーボードのみで開く→プレビュー→差分→確認済みは完了できたが、F6/Shift+F6 で
  ペイン（ツリー↔エディタ↔プレビュー）間のフォーカスが循環しない。
- **根本原因**: `ShortcutAction`（shortcut_table.h）は None/Confirm/ConfirmAll のみで Focus 循環アクションが
  無い。`on_char_hook`（main_frame.cpp）も `WXK_F6` を処理しない（コメントに「F6 循環」とあるのみ）。
- **対応方針**: on_char_hook で F6/Shift+F6 を捕捉し、ツリー/アクティブエディタ/プレビューの順（Shift で逆）
  に `SetFocus` で循環。プレビュー未生成時はスキップ。controller に FocusCycle アクションを足すか UI 直結。
- **状態**: 未対応（G章一括でユーザー判断）

## F-019 単一インスタンス転送のクライアント終了コードが 0 でなく 255（H1/H4）

- **重大度**: 低〜中（転送・前面化は正常動作。終了コードのみ仕様不一致。シェル連携 `pika f && next` で
  next が走らない等の副作用。要件3.4・design 5.1「終了コード0で終了」）。
- **対応章**: H1（転送）/H4（転送時 exit 0）。実機: 起動済みへ `pika target.md` を転送→既存ウィンドウに
  開き前面化・新ウィンドウは作らず即終了(91ms)。だが**終了コード=255**。
- **根本原因**: `main_gui.cpp` のクライアント経路は `send_to_server` 後に **`return false`**（OnInit=false）。
  wxWidgets は OnInit=false で wxEntry が -1 を返す＝プロセス終了コード 255。コメントは「終了コード0」と
  あるが実挙動と乖離。
- **対応方針**: 転送成功後は exit 0 で終わる。`send_to_server` 後に `std::exit(0)`（wx 起動前なので main
  ループ無し・後始末不要）か、SetExitCode(0)＋適切な経路。送信失敗時の扱い（スタンドアロン昇格 or 非0）は
  別途検討（現状の fail-closed 縮退と整合させる）。
- **修正**: `main_gui` クライアント経路を `if (send_to_server(...)) std::exit(0);`＋送信失敗時のみ `return false`
  に変更。`send_to_server` は bool（pipe_server.h）。実機: `pika README.md` 転送で **exit code=0**。
- **状態**: ✅ 修正済み・実機検証済み

## F-020 `-g doc.md:120` の行ジャンプが未適用（H2）

- **重大度**: 中（要件3.1/3.4 の `-g` 行指定オープンが効かない。ファイルは開くがカーソルが指定行へ移動
  しない）。
- **対応章**: H2。`core/ipc` の `OpenTarget{ path, line, column }` は line/column を保持し parse もするが、
  GUI が捨てている。
- **根本原因**: `main_gui` の受信ループと `MainFrame::apply_open_targets(vector<string>)` が **path のみ**
  使い line/column を渡していない（apply_open_targets は `open_file(f)` するだけ）。`main_frame.h` の
  コメントも「line/column は将来のカーソル移動用（本 sprint はファイルを開くまで）」と未実装を明記。
  起動時の `file_paths(plan)` 経路も同様に line を落とす。
- **対応方針**: 転送 JSON と起動 plan から line/column を `apply_open_targets` まで運び、`open_file` 後に
  EditorPanel へ行ジャンプ（`set_caret_position` 相当＝行→オフセット変換 or Scintilla GotoLine）。1始まり・
  0=指定なし。goto_mode はソースモード固定の意図（要件3.1）も併せて反映を検討。
- **修正**: `EditorPanel::goto_line(line, column)` を追加（GotoLine/FindColumn＋EnsureVisibleEnforcePolicy＋
  ScrollToLine）。`apply_open_targets` を `vector<OpenTarget>＋goto_source` 受けに変更し、open_file 後に
  `t.line>0` なら active_editor()->goto_line。`main_gui` の受信ループ/起動経路とも OpenTarget を直渡し
  （`req.goto_mode`/`plan.goto_mode` を goto_source として伝播＝-g はソース表示固定）。`file_paths` ヘルパ削除。
- **状態**: ✅ 修正済み・実機検証済み（`pika -g long.md:120` 転送→120行目表示・カーソル該当行・ソース表示）

## F-021 ファイルツリーが1階層のみ（サブフォルダを展開しても空・ナビゲート不可）

- **重大度**: 中〜大（ワークスペースのサブフォルダ内ファイルを開けない＝AIエージェント出力がサブフォルダ
  にあると確認できない。要件4.1 のツリー機能の中核）。
- **対応章**: I1 検証中に判明（img/ を展開しても sample.png が出ない）。I 章の画像/サブフォルダ系全般に波及。
- **根本原因**: `MainFrame::enumerate_shallow_tree` が `app::list_directory(root, "", ...)`（**単一階層のみ・
  非再帰**）で root 直下だけを列挙し `build_tree` に渡す。サブフォルダはフォルダノードとして出るが children
  が空。`FileTreeModel` は `set_root` の静的 TreeRowVm を使い**展開時の遅延列挙(lazy load)が無い**ため、
  サブフォルダを開いても中身が出ない。「shallow」の名のとおり意図的に1階層。
- **対応方針（候補）**: (A) 遅延列挙＝wxDataViewModel の展開イベントでサブフォルダを `list_directory(root,
  rel)` し children を追加（軽い・大規模ツリーに強い）。(B) 起動時に再帰列挙して全 TreeRowVm を作る
  （単純だが巨大ツリーで重い＝設計原則「軽い」に反しうる）。exclude（.git/node_modules）と監視（深い階層の
  watcher 範囲）との整合に注意。推奨は (A) 遅延列挙。
- **修正**: (B) 再帰列挙を採用。`app::enumerate_tree(root, exclude, max_nodes, capped)` 新設
  （`recursive_directory_iterator`・exclude ディレクトリは `disable_recursion_pending` で降りない・cap=50000 で
  打ち切り `wxLogWarning`・リンク非追従）。`enumerate_shallow_tree`→`enumerate_tree` 差し替え→`normalize_entries`
  →`build_tree`（入れ子 rel_path 対応）でフルツリー化。watcher は既に bWatchSubtree=TRUE で再帰監視済み。
- **状態**: ✅ 修正済み・実機検証済み（img/sample.png・sub/child.md がツリーに見え、sub/child.md を開ける）

## F-022 画像簡易ビュー(I1)・巨大画像ガード(I2)・バイナリ非対応表示(I9)が GUI 未配線

- **重大度**: 中（要件12.2・design 10章 B3。画像/バイナリを開くと**テキストとして開く**＝画像は表示できず
  バイナリは空/文字化け。データ損失ではない）。
- **対応章**: I1（ラスター画像の簡易ビュー）/I2（巨大画像ガード）/I9（その他バイナリの非対応表示）。実機:
  binary.bin が空テキストエディタで開く・sample.png も（ツリーに出れば）テキスト扱いになる。
- **根本原因**: `MainFrame::open_file` が**常に EditorPanel（テキスト）で開く**（decode_auto→set_text_utf8）。
  種別分岐が無い。縮退/分類ロジック `controller/degrade_model`（`DegradeKind::ImageTooLarge` 等・is_image・
  ピクセルガード）は実装＆gtest 済み（系統A）だが open_file が未使用。画像簡易ビュー（wxImage 自前描画・
  WebView2 非起動・フィット/等倍）やバイナリ「対応していない形式＋既定アプリで開く」ビューの UI が無い。
- **対応方針（候補）**: open_file で拡張子/ヘッダから種別判定→(1) ラスター画像=wxImage 簡易ビューア panel
  （フィット/等倍トグル・degrade_model でヘッダ寸法>上限なら ImageTooLarge 誘導＝デコードせず「既定アプリで
  開く」）、(2) その他バイナリ=「対応していない形式」＋「既定のアプリで開く」ボタンの簡易 panel、(3) テキストは
  従来どおり EditorPanel。is_image/ピクセル判定・ガードは degrade_model を結線。
- **修正**: `open_file` に種別分岐を追加（`controller::open_view_model::resolve_open_view`＝画像拡張子→
  ヘッダ寸法(`util::image_header` PNG/GIF/BMP 固定オフセット・他は 64MB フォールバック)→`resolve_degrade`
  でピクセルガード／非画像は `util::binary_detect`(NUL/制御文字 heuristic)→バイナリ／他テキスト）。
  `ImageViewPanel`(wxImage 自前描画・フィット/等倍トグル・WebView2非起動)・`UnsupportedViewPanel`
  (種別ラベル＋既定アプリで開く)を新設。非 EditorPanel タブは保存/dirty/差分が既存ガードで no-op、
  `active_content_class` を content_object_allowed=false に補強。MsgId 3件追加・`wxInitAllImageHandlers`。
  テスト32件追加(OpenViewModel 12・ImageHeader 10・BinaryDetect 10)。
- **状態**: ✅ 修正済み・実機検証済み（sample.png が画像表示＋等倍トグル／binary.bin が非対応表示。ctest 702件 PASS）

## F-025 K章（About/インストーラ/SmartScreen）＝配布・リリース準備が未実装

- **重大度**: 低（配布工程＝アプリ機能ではない。dev/spec.md で「非対象」明記）。
- **内訳**:
  - **K1 About画面**: `on_about` がアプリ名のみの最小 wxMessageBox。バージョン・サードパーティ OSS
    ライセンス表示なし（`assets/THIRD_PARTY_NOTICES` 不在）。
  - **K2 配布**: `installer/`(Inno Setup スクリプト)・ポータブル zip 生成が無い。
  - **K3 SmartScreen**: `docs/install.md`(未署名起動手順)が無い。
- **対応方針**: リリース準備フェーズで、About に version＋THIRD_PARTY_NOTICES（assets 同梱・vendor.lock
  と整合）、Inno Setup（ユーザー単位・管理者不要）＋ポータブル zip、docs/install.md（SmartScreen 回避）。
- **状態**: 未対応（リリース準備工程・ユーザー判断）

## F-024 I章の残り（読取専用誘導/空状態3分岐/診断ログ/FSエッジ）の GUI 結線が未/部分（ユーザー判断＝後回し）

- **重大度**: 低〜中（縮退ロジック `controller/degrade_model`・`view_state` は系統A gtest 済み。GUI 結線が
  欠ける/手動再現困難。データ損失なし）。
- **対応章/内訳**:
  - **I3**: 読取専用ファイルの保存時「名前を付けて保存/属性解除」誘導（degrade ReadOnly→SaveAsOrUnlock）が
    未配線。ただし G10 の保存失敗モーダル「書き込み権限の確認を」で部分カバー。
  - **I4**(AccessDenied)/I6(NetworkDrive)/I7(CloudPlaceholder): degrade_model のロジックは gtest 済みだが
    open_file/列挙経路での GUI 結線が未or部分。実環境(排他ロック/NW/OneDrive)での手動再現も困難。
  - **I5**: `enumerate_tree`(F-021) は recursive_directory_iterator でリンク非追従＝無限展開はしない（安全）。
    degrade SymlinkLoop の明示通知は未配線。
  - **I8**: ワークスペース消失→空状態は F2 で部分確認。ドライブ切断の手動再現困難。
  - **I10**: 空状態が `EmptyNoFolder` の1文言のみ。view_state の3分岐（フォルダ未オープン/検索0件/消化後）の
    GUI 結線が未＝文言が状況で変わらない。
  - **I11**: ファイルログ未実装（診断は OutputDebugString のみ・main_gui に「ファイルログ未配線」コメント）。
    5MB×3世代ローテーション・「ログフォルダを開く」メニューが無い（要件12.3・dev sprint8 should）。
- **対応方針**: 各々 degrade_model/view_state の結線＋UI（保存誘導ダイアログ・空状態文言分岐・ファイルロガー
  ＋ログメニュー）。I章の中核（画像/バイナリ/巨大画像ガード＝I1/I2/I9）は F-022 で完了済み。
- **状態**: 未対応（ユーザー判断「全部後回し・J章へ」）。実装可否は後日判断。

## F-023 最後のタブを閉じるとクラッシュ（wxAuiNotebook 空ノートブック／イベント中の構造変更）

- **重大度**: 大（クラッシュ＝アプリ異常終了。タブを全部閉じる操作で必ず発生）。F-021 実機検証中に判明。
- **再現**: テキスト2ファイルを開く→1つ閉じる（正常）→最後のタブを×で閉じる→一瞬フリーズ後に異常終了。
- **診断（一時トレースで確定）**: 最後の `on_notebook_page_close` は正常完了、その後 `page_changed` が一切
  発火せずクラッシュ＝**wxAuiNotebook が最終ページ削除→空ノートブックになる内部処理でクラッシュ**。
  第1次対処で「空にしないプレースホルダ方式」を入れたが**空状態が一瞬見えた後にまたクラッシュ**＝
  **PAGE_CLOSE イベント処理中に AddPage（プレースホルダ追加）して wxAUI 内部状態を壊した**第2の原因が判明。
- **修正**: (1) `on_notebook_page_close` で**最後の実タブ**は `evt.Veto()`＋`CallAfter` でイベント外
  クローズ（`close_last_real_tab_deferred`：ensure_placeholder→tabs_.close→DeletePage の順で 0 ページを
  経由しない）。実タブ2枚以上は従来の Skip。＝**PAGE_CLOSE イベント中にノートブック構造を一切変更しない**。
  (2) 空状態プレースホルダ（閉じ不可・TabManager 非登録・末尾固定・型分離）でノートブックを 0 にしない。
  (3) update_preview の再入ガード・空早期 return は防御として残置。(4) 恒久のクラッシュハンドラ
  （`SetUnhandledExceptionFilter`＋DbgHelp で `%TEMP%\pika-crash.log` にスタック記録・ヒープ非使用・内容/
  パス非出力＝要件12.3）と Release PDB 生成（CMake `/Zi`＋`/DEBUG /OPT:REF /OPT:ICF`・最適化維持）を追加。
- **状態**: ✅ 修正済み・実機検証済み（最後のタブ×閉じで空状態が安定表示・クラッシュなし・再オープン可。
  ctest 670 件 PASS）

| 項目 | 結果 | 根拠 |
|------|------|------|
| H5 CLI `--version`/`--help` | ✅ | exit 0・パイプ出力欠落/化けなし・制御がシェルへ戻る |
| J1 ワークスペース非汚染 | ✅ | フィクスチャ folder 内に pika 管理ファイルなし（起動・監視後も） |
| A9 WebView2 遅延起動 | ✅ | プレビュー未使用時、pika 由来の WebView2 ユーザーデータフォルダ・データルート未生成 |

## F-026 A章（性能ゲート）は専用の性能計測パスが必要

- **重大度**: 中（リリース前ゲート。要件2.1）。機能は B〜J で検証済み。本件で in-app 計測を実装し
  ベースラインを取得した。
- **実装（in-app 計測 instrumentation）**: `src/app/perf_log.{h,cpp}`（PerfLog ファサード）を新設。
  - **タイミング（A1-A6）**: `QueryPerformanceCounter` で各マイルストーンを begin→end でスタンプし
    区間 ms を算出。計測点は main_gui（A1 起動→表示）・main_frame（A4 切替/A6 外部変更反映）・
    preview_view（A2 Resume/A3 初回プレビュー）。
  - **メモリ（A7/A8）の分離計測（前回ブロッカー解消）**: `GetProcessMemoryInfo`(psapi) で自プロセス、
    `CreateToolhelp32Snapshot` で自分の子孫プロセス（msedgewebview2.exe 等）を再帰列挙し WorkingSetSize
    を合算＝pika プロセスツリー全体のメモリ。他アプリの WebView2 と混ざらない。→ PowerShell/wmic 不要。
  - **原則③（軽い）**: 既定オフ。`--perf-log`（任意 `--perf-log=<path>`・既定 `%TEMP%\pika-perf.log`）
    指定時のみファイル I/O。CLI パーサ（core/ipc・gtest済）・転送 JSON・役割決定は不変（main_gui で
    フラグを拾い collect_args から除外）。Win32 は app/UI 層に閉じ core/ 非汚染。プライバシー：ログに
    パス/内容を書かず、マイルストーン名・ms・基準・PASS/FAIL・メモリ(MB/プロセス数)のみ。
- **ベースライン計測結果（基準機 Release・`--perf-log`・F-004 を足す前）**:
  - A1 起動→表示 **59.4ms**/≤500 ✅・A2 Resume **8.9ms**/≤300 ✅・A3 初回プレビュー **383.6ms**/≤2000 ✅・
    A4 切替 warm **7〜10ms**/≤300（目標150）✅（初回 cold のみ 353.3ms＝キャッシュ構築・実質A3相当）・
    A6 外部変更反映 **175.0ms**/≤500 ✅・A7 メモリ **278MB**/≤350 ✅（自32MB＋子孫7プロセス合算）。
  - **A5 計測対象不在**: ライブ編集→デバウンス→プレビュー更新の**機能自体が未実装**（EditorPanel は
    savepoint のみ束ね・on_editor_dirty_changed は update_preview を呼ばない）。原則④に従い計測の
    ために機能を足さず所見化。実装可否は要ユーザー判断。
  - **A8 未取得**: `--perf-log` 限定の計測補助（Source 畳み後アイドルで強制 TrySuspend）を実装したが、
    **2回の実機試行（1.5秒/3秒以上待機）とも未発火**で A8 取得できず。A2 resume は毎回記録されるのに
    TrySuspend 完了→`record_memory(MemoryIdleAfterSuspend)` 経路が走らない＝補助に潜在不具合。ただし
    **production は遊休サスペンドを駆動しない**（`suspend_if_idle` 呼び出し元ゼロ＝DEC-02 自動サスペンド
    未配線）ため、補助を直しても実運用の数値にはならない。A8 ゲートは **DEC-02 を production で駆動するか
    否かの判断とセット**（要ユーザー判断）。低価値な補助デバッグは保留。
  - **A9** ✅ 済（プレビュー未使用で pika 由来 WebView2 未起動）。**A10** 8時間ソークは dev/spec.md 非対象。
- **状態**: ✅ instrumentation 実装・ベースライン計測済み（A1-A4/A6/A7 PASS）。残：A5（ライブ編集
  プレビュー機能未実装＝要判断）・A8（計測補助が2回とも不発＋production 遊休サスペンド未駆動＝DEC-02 を
  production で駆動するかの判断とセット＝要判断）。

---

## B11 仕様変更 分割＋差分ON＝「プレビュー＋差分」へ（実機検証中のユーザー決定）

- **重大度**: 低（不具合でなく仕様の見直し。レビュー体験の改善）。
- **対応章**: B11（差分トグルON×各モードの実描画）。
- **経緯**: 実機検証中、「分割＋差分ON」が従来「エディタ（Scintilla）＋差分面」だったが、本ツールは
  AI 出力の差分レビュー伴走が用途で、分割＋差分で対比したいのは**生ソースより整形プレビューと差分**。
  ユーザー決定で「分割＋差分ON」を **「左＝整形プレビュー／右＝差分面」**（＝プレビュー＋差分ON と
  同一表示）へ変更（案A採用）。生ソースの編集は「ソースモード」または「分割＋差分OFF」で行う。
- **実装方針（原則③ 軽い／④ 足さない）**: 2つ目の WebView2 を作らず、既存の**プレビュー＋差分グリッド**
  （`build_preview_diff_grid_document`・`body.preview-diff-grid` の CSS 2列・単一 WebView）を流用。
  `controller::resolve_pane_layout`（`src/controller/diff_mode_model.cpp`）の「分割＋差分ON」ケースを
  `show_editor=false`＋`show_preview=true`＋`show_diff=true`＋`preview_diff_grid=true`（＝プレビュー＋
  差分ON と同一 layout）へ変更。`update_preview`（`src/ui/main_frame.cpp`）は無改修で合流する
  （`need_both=show_editor&&webview_active`→false で notebook_ を Unsplit、`grid=true` で grid 文書）。
  分割＋差分OFF は従来どおり「左＝エディタ／右＝整形プレビュー」を維持（差分 OFF で編集分割へ復帰）。
- **正典 docs 改訂**: ui-design 8章（対応表）／acceptance.md B11 行／本書。design 6章は WebView2 1枚共有
  ＝grid 流用と整合するため改訂不要。
- **gtest**: `tests/controller/diff_mode_model_test.cpp` の Split＋diff 期待値を新仕様へ更新
  （`SplitWithDiffUsesPreviewDiffGrid`）。グリッド文書生成（`preview_builder_test.cpp`）は流用で不変。
- **状態**: 実装済（コミット前・作業ツリー）。**実機再確認前（未確定）**＝分割＋差分ONで左=整形/右=差分、
  差分OFFで編集分割へ復帰、ソース+差分（差分面のみ）・プレビュー+差分（grid）が不変、を実機確認待ち。

---

# === Tauri フル刷新フェーズ（系統C 所見・sprint 1〜）===

> 以下は wxWidgets/C++ 版から Tauri（Rust + WebView2 + TS）へのフル刷新フェーズの所見。
> 旧 wx 前提の F-xxx 所見は新スタックで再検証扱い（design doc 16章）。検証環境は基準機の
> Release ビルド（`cargo build --release` ＋ `npm run build` 後の `pika.exe`）。
> 本フェーズの系統C 必達項目は dev/spec.md「検証戦略 系統C」と design doc 15章 Open に対応する。

## T-001 IPC コスト実測（design doc 15章-1・3章／要件2.1）

- **重大度**: 高（性能目標 2.1 の根幹。転送方式を誤ると更新300ms/初回2.0秒を構造的に外す）。
- **対応章**: design doc 3章「IPC コスト予算」・15章-1／要件2.1。
- **検証内容**: プレビュー相当 HTML を (a) `invoke` で JSON 文字列として返す経路と
  (b) custom protocol（`pika-preview://`）が別WebView へ直配信する経路でラウンドトリップを実測し、
  64KB〜数MB のペイロードで更新300ms・初回2.0秒を満たす転送方式を確定する。
  保存（編集バッファ全量）は Channel API、巨大ファイル range 読取は custom protocol range を測る。
- **設計上の確定**: design doc 3章の予算表に従い、プレビュー HTML は **custom protocol 直配信**（invoke で返さない）、
  保存は Channel、ツリー/小データのみ invoke(JSON)。本スプリント（sprint 1）の最薄ループでは
  保存をテキスト invoke で暫定配線し、本計測でこの予算への適合を確認する。
- **状態**: 設計確定・実装経路（custom protocol/Channel の置き場所）を sprint 4/6 で本配線。**基準機 Release での
  ラウンドトリップ実測は未実施（要実機）**。実測値を本欄に追記する。

## T-002 別WebView 権限ゼロ隔離の実証（design doc 15章-3・6章）

- **重大度**: 高（設計の根幹。1つでも到達したら設計やり直し）。
- **対応章**: design doc 6章「隔離方式: 権限ゼロの別 WebView」・15章-3。
- **検証内容**: プレビュー用の別WebView を capability ゼロ（command 無し・`core:default` 不付与）で生成し、
  その WebView 内から `invoke(...)` / `window.__TAURI_INTERNALS__` 経由の任意 command が **到達不能**で
  あることを Windows 実機 Release で確認する。1つでも到達したら設計やり直しとする。
- **設計上の確定**: capability マップ（design doc 9章）で、メインウィンドウ（label "main"）にのみ
  `capabilities/main.json` を割り当て、プレビュー WebView（label "preview"・sprint 4 で生成）には
  **capability ファイルを置かない**。Tauri は未宣言ウィンドウへ command を一切許可しないため、
  プレビュー WebView は権限ゼロになる（src-tauri/src/main.rs の run() コメント参照）。
- **状態**: capability マップの土台を sprint 1 で確立（main のみ最小・preview はファイル不在＝ゼロ）。
  **プレビュー WebView の実生成と到達不能の実機実証は sprint 4 の本番経路で実施**（本スプリントは経路設計の確立）。

## T-003 WebView2 不在時起動（design doc 15章-5・18章／要件2.3 改訂）

- **重大度**: 高（不在時にウィンドウすら描けず、最小ダイアログが無いと無反応終了になる）。
- **対応章**: design doc 18章「WebView2 不在時のフォールバック」・要件2.3 改訂。
- **検証内容**: WebView2 Runtime 不在/破損時、Tauri 起動「前」に Win32 MessageBox（WebView 非依存）で
  Evergreen Runtime の導入を案内して終了する。不在環境を模擬（HKLM/HKCU の Evergreen GUID キー `pv` を
  退避/改名）して手動確認する。
- **実装**: `src-tauri/src/webview2.rs` に実装。`ensure_runtime_available()` が
  HKLM\...\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}\pv（64bit ビュー）または
  HKCU\... の同キーを読み、非空（"0.0.0.0" 以外）の版があれば導入済みと判定。不在なら `main()` が
  Tauri 起動前に `show_missing_runtime_dialog()`（`MessageBoxW`）で案内し `exit(1)`。
- **状態**: 実装済（cargo build 成立）。**不在環境を模擬した実機手動確認は未実施（要実機）**。模擬手順と
  ダイアログ表示・終了コードの実測結果を本欄に追記する。

## T-004 最薄ループの貫通（sprint 1・中心体験①／系統C）

- **重大度**: 高（以後の全スプリントが土台にする）。
- **検証内容**: `pika.exe` が起動し、フォルダパスを入力→`invoke('open_workspace')`→ツリー表示→
  ファイルをクリックでタブ＋CM6 を開く→編集（dirty 化）→「保存」で `save_file`（現状 invoke 配線）まで
  手で通す。GUI 実機が要るため系統C。
- **状態**: 実装済（cargo build＋npm build 成立・cargo test PASS）。**基準機での実機起動と一連の貫通操作は
  未実施（要実機）**。起動時間・初回プレビューは T-001 と併せて計測する。

## T-005 watcher オーバーフロー表面化・rename FileId 補強（sprint 2・design doc 15章-2／系統C）

- **重大度**: 高（要件7.4「100件同時で取りこぼさない」が中心シナリオ。握り潰すと未読を失う）。
- **対応章**: 要件7.1/7.4・design doc 4章/15章-2・19章 rename継承。acceptance.md TC1〜TC6。
- **検証内容**:
  1. **notify オーバーフロー表面化** — notify crate（v6.1.1）が `ReadDirectoryChangesW` のバッファ溢れ
     （ERROR_NOTIFY_ENUM_DIR ＝ Win32 0x3FF）を `Error`/`EventKind` で表面化するか実機で確認する。
     表面化すれば `src-tauri/src/watcher.rs::is_overflow_error()` が拾って `do_overflow_resync()`（全再列挙）へ落ちる。
     **握り潰す（個々のイベントだけ来てバッファ溢れは黙殺される）なら、windows crate `ReadDirectoryChangesW` 直叩きへ
     切替える**（design doc 50行/341行）。判断結果を本欄に追記する。
  2. **100件同時変更** — 配下に 100 ファイルを一斉生成/書換しても全件未読化されることを実機確認する。
- **設計上の確定（決定論側は cargo test 済み）**:
  - 合成層は **pika-core::watcher**（UI/notify 非依存）に集約し `cargo test` で固めた（28 件）。
    `overflow::resync_against_baseline` は「新規/削除/変更」を昇順・決定論で算定し、`百件同時新規を全件取りこぼさない`
    テストで 100 件取りこぼし無しを観測（オーバーフロー後の全再列挙が機能する保証＝決定論側）。
  - `is_overflow_error()` は notify の `ErrorKind::Generic("...overflow...")` と
    `ErrorKind::Io(raw_os_error == 0x3FF)` の双方を拾う実装にし、notify がどちらの形で出しても再同期へ落ちる。
    どちらも出さない（黙殺）場合の切替判断を実機で確定する。
- **rename FileId 補強の制約**: `std::fs::Metadata` の `file_index()`/`volume_serial_number()` は安定版 Rust では
  未提供（nightly `windows_by_handle`）。そのため `src-tauri/src/watcher.rs::file_id_of()` は **安定版では `None`** を返し、
  rename ペア化は **時間窓ベース**（pika-core::watcher::rename の段2）に倒している。相互スワップ/上書き rename の
  完全な解決には FileId が要るため、**windows crate `GetFileInformationByHandle` の導入を sprint 5/7（または本 T-005 の
  切替時）に検討**する。pika-core 側の正規化ロジックは FileId が来れば段1（FileId 主キー）で解決する設計済み
  （`相互スワップ_a_b_を_2_本の_rename_に正規化する` 等のテストは FileId 有りで合格）。
- **状態**: 合成層は cargo test PASS・配線（監視スレッド/emit/ポーリング/F5）は cargo build 成立。
  **基準機での 100 件同時変更・オーバーフロー再現・notify の表面化有無・rename 実挙動は未実施（要実機）**。
  実測結果と FileId 補強の要否判断を本欄に追記する。

## T-006 スナップショット/差分/確認済み（sprint 3・中心体験③④／系統C）

- **重大度**: 高（最上位原則1「データを失わない」＝退避が最後の砦。確認済み/巻き戻しは破壊的操作）。
- **対応章**: 要件8.2/8.3/8.4・9.1/9.2/9.3・7.3・design doc 7章/11章・19章 衝突退避不能ガード/状態復元。
  acceptance.md TD1〜TD9。
- **決定論側（cargo test 済み・50 件）**:
  - **diff** — `pika-core::diff`（similar Myers＋語/grapheme フォールバック）。LF 正規化照合で改行のみの差を
    出さない・空ファイル・新規=全行追加・全削除・累積差分・置換行の行内セグメント・日本語の文字単位強調・
    結合文字を grapheme 境界で壊さない を観測。**自前なのは語境界不成立行→grapheme 切替の判定のみ**（design doc 7章）。
  - **snapshot** — `pika-core::snapshot`。content-addressed object（twox-hash LF 正規化ハッシュ・改行のみ違いは同一 object）・
    zstd 往復・退避4種（conflict/incoming/rollback/baseline-replace）の自己記述メタ・**index 破損時に object メタから
    退避一覧を再生成**（実体欠落メタは復元しない）・ベースライン常に1件・**ハッシュのみは差分巻き戻し非対象**を観測。
  - **容量管理** — `pika-core::snapshot::gc`／`store`。ファイルごと最新10件 LRU（未復元＞復元済み＞新しい）・
    全体500MB＋90日 stale 判定・**未復元かつ14日以内は容量GC保護**（保護のみで上限超過なら削除せず超過バイトを返す）・
    **共有 object は全参照（baselineHash/stash.hash）不在を確認後にのみ物理削除**・baseline-replace は10件枠と別 を観測。
  - **機密/10MB境界** — `pika-core::snapshot::policy`。`.env`/`.env.*`/`*.key`/`*.pem`/`*secret*`・画像・**ちょうど10MBを含む
    10MB以上**はハッシュのみ、10MB未満テキストのみ内容保存 を観測（境界＝「10MB未満のみ内容保存」）。
  - **確認済み/巻き戻し判定** — `pika-core::review`。**確定直前の mtime/ハッシュ再照合で変化を検知し中断（再差分）**・
    すべて確認済みは**実行開始時点をフリーズ・変化ファイルをスキップ（未読維持）・更新前を baseline-replace 退避**・
    巻き戻しは**ベースライン内容あり かつ 現在内容退避可能**でのみ許可・**退避不能（10MB以上/画像）は既定ブロック**
    （設定で強い確認のうえ許可）を観測。**退避（object 保存）失敗時はベースラインを進めず未読維持＝退避を握り潰さない**。
- **配線（cargo build＋npm typecheck＋vite build 成立）**:
  - command: `compute_file_diff`／`confirm_file`／`confirm_all`／`rollback_file`（src-tauri/src/snapshot.rs）。
    ロジックは全て pika-core 委譲・command は薄い境界（FS 読取＋DTO 化）。
  - frontend: `src/diff/index.ts`（read-only unified レンダラ・行頭±記号・変更語下線/太字・色非依存・F8/Shift+F8）。
    `src/main.ts`（差分トグル Ctrl+\・Ctrl+E でソース復帰・確認済み/すべて確認済み/巻き戻しボタン）。
- **本スプリントの限定（後続で解消）**:
  - ベースライン内容 object は **メモリ保持で結線**（中心体験貫通を優先）。データルート配下への zstd 永続化＋
    windows crate 厳格 DACL は後続で同じ pika-core 判定（policy/gc/store）を再利用して実装する（design doc 11章）。
    したがって TD8（index 破損復元）・TD9（容量GC）の**実機検証は永続化実装後**。決定論ロジックは cargo test 済み。
- **状態**: 決定論ゲート（cargo test 92 件）PASS・cargo build／npm typecheck／vite build 成立。
  **基準機での差分実描画・確認済み/巻き戻し実操作・実 FS のベースライン/退避・退避不能ガード実挙動は未実施（要実機）**。

## T-007 プレビュー（権限ゼロ別WebView・最重要セキュリティ境界／sprint 4・系統C）

- **重大度**: 最上位（補助原則「未信頼コンテンツはアプリシェルから物理隔離」＝全Web化の最重要設計。
  XSS→invoke→実質RCE を構造的に排除する境界。design doc 6章/15章-3）。
- **対応章**: 要件6.1/6.2/6.3/6.4・2.2（暴走ガード）・9.1（機密配信拒否）・design doc 3章/6章/7章/15章-3。
  acceptance.md TE1〜TE8。
- **決定論側（cargo test 済み・40 件・pika-core::render）**:
  - **sanitize** — `pika-core::render::sanitize`。comrak(`unsafe_=true`)→ammonia 最終段サニタイズ。
    `<script>`・`on*` 属性・`javascript:`/scriptable `data:` URL・`<iframe>/<object>/<embed>/<base>/<meta>`
    （`<meta http-equiv refresh/CSP>` 含む）除去・**`id`/`name` 制限（DOM clobbering 防止）**・
    **SVG サブセット**（`script`/`foreignObject`/`on*`/`xlink:href javascript:` を明示禁止）を観測。
    系統A=Markdown/差分/SVG・系統B=HTML（インライン CSS 尊重・JS 無効）。**comrak の raw HTML 内 script も
    最終段で必ず除去**（多層の要）を観測。
  - **csp** — `pika-core::render::csp`。レスポンスヘッダ用 CSP 組立。系統A=`script-src 'nonce-<rnd>'`／
    系統B=`script-src 'none'`。`default-src 'none'`・`connect-src 'none'`・`object-src 'none'`・`base-uri 'none'`・
    `frame-ancestors 'none'`。**オプトイン緩和は img-src/font-src への許可ホスト追加に限定**し
    script/connect/object は緩めず、**緩和を空にすると既定の外部遮断に戻る**ことを観測。nonce は CSPRNG・毎回異なる。
    script-src に `'unsafe-inline'` を付けないことを観測。
  - **path** — `pika-core::render::path`。ローカル相対参照の封じ込め判定（FS 非依存）。`../`/絶対パス/
    ドライブ指定/UNC/**パーセントエンコード経由の脱出**（`%2e%2e%2f`）・機密ファイル（`.env`/`*.key`/`*.pem`/
    `*secret*`）・空参照を拒否。canonicalize 済みパスの prefix 検証（`confine_under`）でシンボリックリンク脱出を弾く
    （実 canonicalize は src-tauri の `local_resource_response` が実行）。
  - **guard** — `pika-core::render::guard`。暴走ガードを入力段で計測。画像6000万px・SVG8000万px/5万要素を
    超過でブロック（寸法乗算は saturating でオーバーフロー回避）・HTML10秒タイムアウト値供給・長行ガード（10万字）を観測。
- **配線（cargo build＋npm typecheck＋vite build 成立）**:
  - protocol: `register_uri_scheme_protocol("pika-preview", handle_preview_request)`（src-tauri/src/preview.rs）。
    `/doc/<gen>` がサニタイズ済み HTML を CSP ヘッダ付きで配信・`/local/<gen>/<相対>` が封じ込め検証してローカル
    参照を配信。**prepare_preview は URL のみ返し HTML 本体を invoke に乗せない**（IPC 予算＋オリジン分離）。
  - command: `prepare_preview`（系統 A/B 分岐＋ pika-core::render 委譲・**系統B の危険検知 hazards を戻りに同梱**＝
    content の二重 invoke を避ける）。要件6.3 の危険検知は prepare_preview の `hazards` フィールドで返す
    （iteration2 で独立 `scan_html_hazards` command を廃し 1 invoke に統合）。
  - frontend: `src/preview/index.ts`（3モード×差分トグル直交占有・系統A/B 切替の世代直列化 PreviewSerializer・
    信頼 JS 注入スクリプト生成 buildTrustedJsInit〔Mermaid securityLevel:strict／KaTeX trust:false strict:true
    maxExpand／per-block 描画＋約1秒タイムアウト＋失敗件数集計〕・**失敗件数メッセージの受信検証
    parsePreviewFailureMessage**）。`src/main.ts`（プレビュー切替ボタン・占有適用・**失敗件数 message リスナ→通知バー**・
    プレビュー 5状態 data-state）。
  - capability: `capabilities/main.json` は `windows=["main"]` のみ＝**preview ラベルは未宣言で権限ゼロ**（design doc 6章）。
- **iteration2 の修正（eval high/medium 反映）**:
  - **暴走ガード結線（high・要件2.2）**: `preview.rs::local_resource_response` が配信前に `guard_local_resource`
    →`pika-core::render::guard::check_image_bytes`/`check_svg_bytes` を呼び、巨大画像（ヘッダ寸法 6000万px 超）・
    SVG（要素数 5万超 or 推定 8000万px 超）を **413 で配信拒否**する（WebView 任せにしない＝固まらない）。
    画像ヘッダ寸法読取（PNG/GIF/JPEG/BMP/WebP）・SVG 要素数計数は pika-core（cargo test 済み）。配線テスト
    `ローカル配信の暴走ガードが結線されている`（src-tauri）で観測。frontend は 413 をプレースホルダ/通知導線に。
  - **CSP ディレクティブインジェクション対策（high・要件6.2）**: `pika-core::render::csp::validate_allow_hosts`
    （https:// のみ・ホスト名/ポート文字種限定・空白/`;`/クォート/`*`/改行/パス/クエリ拒否）を新設。
    `prepare_markdown_preview`/`prepare_html_preview` が **CSP 組立前に検証し、1 つでも不正なら外部許可を全破棄
    （fail-closed＝既定遮断へ倒す）**。`build_csp` も防御的に不正ホストを連結しない（多層）。cargo test で
    `https://evil.com; script-src *` 等が CSP に漏れないことを観測。
  - **失敗件数の受信導線（high・要件6.2）**: メインWebView に `window.message` リスナを追加し
    `parsePreviewFailureMessage` で型検証して通知バーへ集計。**別WebView は独立 WebView のため window.parent では
    本体に届かず、本番（系統C 結線）では Tauri event/IPC 経路へ置換が必要**（TE6 で追跡）。
  - **退避操作の二重送信防止（high）**: 保存/確認/一括確認/巻き戻しを `withBusy` で in-flight 抑止
    （即時 disabled＋busy フラグ）。連打で confirm_all/rollback_file が並行発火しデータ操作が重複するのを防ぐ。
  - **medium**: has_meta_refresh 通知導線追加・プレビュー 5状態（loading/ready/error）の data-state＋占有 CSS・
    すべて確認済みスキップ時の F5 再同期案内・HTML hazards の IPC 二重転送解消（prepare_preview 戻りに同梱）。
- **本スプリントの限定（系統C で確認）**:
  - 別WebView の実生成/ナビゲート（`ready.url` を別WebView src へ設定）と権限ゼロ隔離の実証（TE2・必達）は
    **Windows 実機 Release**（design doc 15章-3）。本スプリントは protocol/コマンド/サニタイズ/CSP/封じ込めの
    決定論側と配線まで（HTML は非経由の設計）。frontend は占有領域へ `data-preview-url` を保持し、別WebView の
    実アタッチは系統C で結線する。
  - 双方向スクロール同期（should・要件6.1）・プレビュー内検索（should・要件5.4）は注入土台（sourcepos data-line・
    nonce 注入経路）まで。実描画/実検索は系統C（design doc 15章-7）。
- **状態（iteration2）**: 決定論ゲート（cargo test pika-core 149 件・うち render 57 件〔csp/guard 強化〕＋preview 配線
  5 件）PASS・cargo build／npm typecheck／vite build 成立。**別WebView の権限ゼロ到達不能（TE2 必達）・系統A/B 実描画・
  信頼 JS 注入の実挙動・CSP の実効・暴走ガード実挙動・失敗件数の Tauri event 経路は未実施（要 Windows 実機 Release）**。

## T-008 CLI 二段構成・単一インスタンス（自前 named pipe）・状態復元（sprint 5・design doc 15章-9／系統C）

- **重大度**: 高（信頼境界＝named pipe の DACL/`PIPE_REJECT_REMOTE_CLIENTS`/受信引数の core 再検証・受理操作=パスオープン限定。
  最上位原則「データを失わない」＝state.json アトミック書込＋version 安全側＋復元3分岐）。
- **対応章**: 要件3.1/3.2/3.4・10.1・13・design doc 9章/15章-9/19章（状態復元3分岐）。acceptance.md H1〜H7。
- **決定論側（cargo test 済み・pika-core）**:
  - **cli** — `pika-core::cli`。`-g <file>:<行>[:<桁>]` の**ドライブレターのコロン非分割**（`C:\dir\a.md:12:3` の先頭
    `:` を保護し末尾から行・桁を剥がす）・桁省略=行頭・非整数=位置無視・空引数エラー。`normalize_to_absolute`
    （絶対パスはそのまま・相対は cwd 前置・**ドライブ相対 `X:rel` は決定論で絶対化不能のため安全側で拒否**）。
  - **ipc** — `pika-core::ipc`。`decide_role`（CreateNamedPipe の成否＝原子的ロックでサーバー/クライアント決定）・
    `build_pipe_name`（`\\.\pipe\pika-<SID>`・SID にパイプ名注入文字が混ざれば拒否）・`build_forward_message`／
    `parse_incoming_message`（**受信 ≤ 8KB 打切り・JSON スキーマ検証・version 不一致は安全側で拒否・各パスを
    path_verify で再検証・余分フィールド（command 等）は OpenRequest に吸われない＝受理操作=パスオープン限定**）。
  - **path_verify** — `pika-core::path_verify`。受信パスの再正規化・再検証（**相対パス拒否・NUL/制御文字拒否・
    ADS（`file:stream`）拒否・UNC/ドライブ絶対/拡張長パス分類・長パスへ `\\?\`/`\\?\UNC\` 接頭辞付与**）。
    転送パスを信頼せず core 検証層で必ず再検査する（要件3.2）。
  - **state** — `pika-core::state`。AppState の serde 往復・`load_state`（**version を先に覗いて未知=UnknownVersion
    〔読まず/書かず/再生成せず〕・破損=Corrupt〔空起動・既存保全〕・既知=Ok**）・`restore_tab`（**復元3分岐**＝
    消失=削除済み表示／別物=未読復元／一致=正常復元）・`restore_workspace`（消失=空状態へ安全遷移／存在=復元／無=単体）。
- **配線（cargo build＋npm typecheck 成立）**:
  - CLI 二段構成: `crates/pika-cli`（console subsystem）が `--help`/`--version`/引数検証/終了コード（0=受理・2=引数
    エラー・3=GUI 起動失敗）を同期処理し、`-g`/パスを core で絶対パス正規化してから `pika.exe` を spawn（GUI 起動が
    必要なときのみ）。`pika --version` はリダイレクト取得でも素の文字列のみ（文字化け回避）。
  - 単一インスタンス: `src-tauri/src/single_instance.rs`。`CreateNamedPipeW`（`FILE_FLAG_FIRST_PIPE_INSTANCE` の成否＝
    原子的ロック）・**SDDL `D:(A;;GA;;;OW)(A;;GA;;;SY)` で owner/System 限定 DACL**・`PIPE_REJECT_REMOTE_CLIENTS`・
    `PIPE_TYPE_MESSAGE`・受信は `parse_incoming_message`（core 検証）。サーバーは**ウィンドウ表示前に**パイプ公開し、
    クライアントは絶対パス正規化済み JSON を転送→`app.handle().exit(0)`（終了コード0）。受信時は `emit('open-request')`
    ＋既存ウィンドウ前面化（`unminimize`/`show`/`set_focus`）。SID/パイプ名取得不能時は単一インスタンスを諦め
    サーバー扱いで起動継続（縮退）。
  - 状態復元: `src-tauri/src/state_store.rs`。データルート解決（`pika-core::data_root`）→ state.json の**アトミック
    書込（一時ファイル→rename）**→FS を見て PathProbe（消失/別物=ハッシュ照合/一致）を作り core の復元判定を呼ぶ。
    **未知バージョン/破損は `safe_empty=true` で空起動しつつ既存 state.json を上書きしない**（読めない状態を保全）。
  - command: `save_app_state`/`restore_app_state`（src-tauri/src/commands.rs。直列化・version・3分岐は core 委譲）。
  - frontend: `src/main.ts`（起動時 `restoreOnStartup`〔ワークスペース復元/タブ3分岐〔削除済み通知・未読復元・正常開く〕・
    `safe_empty` 中は保存抑止〕・`beforeunload`/フォルダ開く/タブ開くで `persistAppState`・`onOpenRequest` で転送パスを開く）。
    `src/editor/index.ts`（`gotoPosition`＝`-g` 行・桁へカーソル移動・行超過は最終行・桁超過は行末クランプ）。
  - capability: `capabilities/main.json` のメイン最小権限のみ（preview ラベルは未宣言で権限ゼロを維持）。
    pika 独自 command（save/restore_app_state 等）は `generate_handler!` で main webview に許可（外部公開なし）。
- **本スプリントで系統C（Windows 実機）に残す確認**（acceptance.md H1〜H7 を新スタックで再検証扱い）:
  - H1: 起動済みで `pika foo.md`→呼出プロセス即終了・既存ウィンドウに foo.md が開き前面化。
  - H2: `pika -g doc.md:120`→転送先で120行目へカーソル・表示範囲。
  - H4: 転送時に未保存タブ切替確認をキャンセルしても呼出プロセスは終了コード0（受理）。
  - H5: `pika --version`/`--help` のパイプ/リダイレクトで出力欠落・文字化けなし・シェルへ制御が戻る。
  - H6: 異常終了後もロック残留せず正常起動（パイプはプロセス死で OS が解放）。
  - H7: SID 取得失敗時は owner-less パイプを作らずスタンドアロン縮退（制限トークン等は実機で強制困難）。
  - DACL/`PIPE_REJECT_REMOTE_CLIENTS` の実効（別ユーザー/リモートからの接続拒否）は実機で確認。
- **状態（sprint 5・iteration1）**: 決定論ゲート（cargo test pika-core 193 件〔うち本スプリント新規=cli 6・ipc 14・
  path_verify 12・state 14〕＋pika-cli 11・pika-app 5）PASS・cargo build（crates＋src-tauri・警告エラー扱い）／
  npm typecheck 成立。**単一インスタンス転送の実機挙動（H1/H2/H4・即終了/前面化/終了コード0）・named pipe の
  DACL/リモート拒否の実効・state.json 復元の実 GUI 反映は未実施（要 Windows 実機）**。
- **iteration2 で追加（状態復元の実体化・最近使った項目・回復導線・eval feedback 反映）**:
  - **content_hash 実体化**（eval high）: フロントが開く/保存/外部リロード/巻き戻し時に backend
    `hash_content`（`pika-core::hashing::hash_normalized_lf`＝自己保存抑制/復元別物判定と同一規則）でタブの
    内容ハッシュを実値で詰めるようにした。これにより復元3分岐の **別物=未読復元** が production で発火する
    （従来は content_hash 空固定で probe が常に Same を返す死に枝だった）。**カーソル/スクロールも CM6 の
    `getCursor`/`getScrollTop` から実値を収集**して state.json へ保存し、復元時に `gotoPosition`/`scrollToLine`
    で戻す。→ H8（系統C）: タブを途中行で開いた状態で終了→再起動で同じ行・スクロールへ復帰すること。
  - **active_tab 往復**（eval high）: `RestoreOutcomeDto.active_path`（`active_tab` インデックスをパスへ解決）を
    追加し、復元時はタブを全部開いたあと**パスで**再アクティブ化する。→ H9（系統C）: 終了時に見ていたタブが
    再起動後もアクティブで復元されること（最後に開いたタブに引きずられない）。
  - **削除済みタブの回復導線**（eval high・旧 wx F-017 同質の行き止まり防止）: 起動復元で外部削除されていた
    タブを**取消線＋× の削除済みタブとして残し**、空エディタを出して「確認済み時点に戻す（rollback）」へ到達
    可能にした。退避/ベースラインは snapshot に残るので削除済みタブから復元できる。→ H10（系統C）: 開いていた
    ファイルを外部削除→再起動→削除済みタブが残り rollback で退避内容へ戻せること。
  - **起動復元の巨大ファイルガード**（eval high/performance）: `probe_path` を metadata len 先読みにし、
    **10MB 以上は全量読込せず Same 扱い**（起動ホットパス保護・spec「10MB 以上はハッシュのみ」と整合）。
    → H11（系統C）: 10MB 級テキストを複数タブで開いた状態で再起動しても起動 0.5 秒ゲート内であること。
  - **最近使った項目＋ジャンプリスト**（should・要件10.2）: `pika-core::recent::RecentList`（LRU・大文字小文字
    無視の重複排除・各20件上限。cargo test 6 件）を state.json へ保持。`note_recent` command が read-modify-write
    （未知/破損時は保全して何もしない）で更新し、`src-tauri/src/jumplist.rs` の `SHAddToRecentDocs(SHARD_PATHW)`
    で **OS の Recent ジャンプリストへ登録**する（`ICustomDestinationList` のカスタムカテゴリは要件外＝足さない）。
    → H12（系統C）: ファイル/フォルダを開くとタスクバー右クリックの「最近使ったもの」に出ること（実描画は実機）。
  - **フォルダ切替＋未保存確認**（should・要件3.2）: 起動中に別フォルダを指定すると、未保存タブがあれば破棄確認を
    挟んでから前フォルダのタブ/未読/エディタを畳んで切り替える（複数フォルダ同時オープンはしない＝要件14章）。
  - **存在しないパスの扱い**（should・要件3.2）: `path_kind` command で種別判定し、**存在しないファイルパスは
    「保存時に作成される新規タブ」として空タブで開き**、フォルダは切替、存在しないフォルダはエラーにする。
  - **eval medium 反映**: 破損/未知バージョンの空起動を通知バーで明示（前回状態を読めず空起動・既存は保全の旨）／
    複数パス + `-g` 併用時のカーソルを paths 先頭タブへ結びつけ／一括確認の差分先読みを `Promise.all` で並行化
    （N+1 IPC の直列線形劣化を解消）／`persistAppState` を 400ms デバウンス（終了時は即時 flush）。
  - **eval low 反映**: state.json アトミック書込で rename 前に `sync_all`（fsync）し、一時ファイル名に PID
    サフィックスを付けて同居プロセスの tmp 競合を構造的に排除。
  - **ウィンドウ前面化の責務分担（系統C 検証項目）**: 前面化は backend（`single_instance::bring_to_front` の
    `unminimize`/`show`/`set_focus`）が本筋。フロントは `open-request` でファイルを開くのみ（前面化はしない）。
    開いた結果がユーザーに見える保証（アクティブタブへフォーカス移動等）は H1 と併せて実機で確認する。
  - **状態（sprint 5・iteration2）**: 決定論ゲート（cargo test pika-core 207 件〔iteration1 比 +14＝hashing 6・
    recent 6・state +2〕＋pika-cli 11・pika-app 5）PASS・cargo build（警告エラー扱い・exit 0）／npm typecheck
    成立・cargo fmt --check クリーン。上記 H8〜H12 と H1〜H7 は引き続き系統C（Windows 実機）で確認する。

## T-009 巨大ファイル段階制・エンコーディング・検索置換（sprint 6・design doc 8章/15章-6/15章-8／系統C）

- **重大度**: 高（中心シナリオ「AI 出力の単一行巨大 JSON/JSONL」の編集死守＝要件2.2 第1段階・最上位原則
  「固まらない」＝巨大ファイルを CM6 へ全量ロードしない・「データを失わない」＝エンコーディング保存中断で
  無確認の文字欠落を防ぐ）。
- **対応章**: 要件2.2（巨大ファイル段階制・行長ガード）・5.2/5.6（エンコーディング往復・保存中断）・5.4
  （検索/置換）・9.2（内容保存境界）・design doc 8章/15章-6（CM6 実測）/15章-8（fancy-regex 機能テスト）/16章
  （canon 改訂）。acceptance.md L1〜L7（後述）。
- **決定論側（cargo test 済み・pika-core）**:
  - **huge** — `pika-core::huge`。`FileStage::from_size`（**Normal〔10MB 未満〕/Stage1〔10MB 以上・第2段階以下〕/
    Stage2ReadOnly〔第2段階超・上限以下〕/TooLarge〔上限超〕**・境界＝第1段階は 10MB「以上」〔ちょうど 10MB は第1段階・
    9.2 と整合〕、第2段階/上限は閾値「超」）・`can_open/can_edit/can_save/can_replace/auto_off_heavy_features/
    needs_virtual_viewer`・`degrade_flags`（段階制〔サイズ〕と行長ガード〔内容〕を合算した自動オフフラグ）・
    `has_long_line`（1 行 10万字超でハイライト/折返し自動オフ・改行で行ごとリセット・CR は行長に数えない）。
  - **range** — `pika-core::range`。仮想化ビューアの `window_around`（中心の前後をウィンドウ幅で取りファイル端
    クランプ・幅 0 は既定 1MB・サイズ超中心でも破綻しない）・`align_to_lines`（読んだバイト列を**行頭/行末**に整え
    半端な行を前後から削る・先頭ファイルは先頭を削らない・改行なし巨大1行は全体返し）。実 I/O（seek+read）は
    src-tauri 側で算出結果を使う。
  - **encoding** — `pika-core::encoding`。`decode`（**BOM 最優先〔UTF-8/UTF-16 LE/BE〕→ BOM なしは UTF-8→Shift_JIS
    の strict デコードで妥当性検査 → いずれも不正なら UTF-8 lossy で警告**・改行は原文保持）・`classify_line_ending`
    （LF/CRLF/CR/Mixed/None・混在検出）・`encode_for_save`（**元エンコ/BOM 維持・Shift_JIS で表現不能文字があれば
    `SaveOutcome::Unmappable` で保存中断**〔無確認の文字欠落を防ぐ＝要件5.6〕）・`encode_as_utf8`（［UTF-8で保存］
    選択肢）。UTF-8/Shift_JIS の往復がバイト一致・BOM 維持・絵文字 Shift_JIS で中断＋該当文字インデックスを cargo test
    で観測。
  - **search** — `pika-core::search`。`search_all`/`replace_all`（**fancy-regex＝後方参照 `(\w)\1`・キャプチャ参照
    `$1`/`${name}`・Unicode 文字クラス `\p{Hiragana}`** をサポート＝要件5.4 全機能・Scintilla 正規表現不採用の理由を
    解消）・大文字小文字区別/単語単位/リテラル（メタ文字エスケープ）の各オプション・**ReDoS バックトラック上限
    〔`backtrack_limit` 100万・catastrophic backtracking でハングせず `SearchError::Backtrack`〕＋協調キャンセル
    〔`Cancel` フラグで途中打切り・別ハンドルからも効く〕＋件数上限〔10万件で truncated〕**・空マッチで無限ループしない
    前進保証・キャンセル時も内容を失わない。**fancy-regex は要件5.4 全機能を通過したため第一候補で確定（pcre2 フォール
    バック不要）＝design doc 15章-8 の判断**。
- **配線（cargo build＋npm typecheck 成立・`src-tauri/src/document.rs`／`src/ipc.ts`）**:
  - `open_document`（サイズで段階判定→上限超はエラー・第2段階以降は内容を返さず段階だけ返す〔CM6 へ全量ロードしない〕・
    通常/第1段階はエンコーディング判定してデコード済みテキスト＋縮退フラグを返す）。
  - `save_document`（エンコーディング維持保存・Shift_JIS 表現不能で `status:"unmappable"` を返し中断〔フロントは
    ［UTF-8で保存/該当文字を確認/キャンセル］提示〕・`force_utf8` で UTF-8 書込・**アトミック書込〔一時ファイル→rename〕**・
    自己保存抑制トークン登録）。
  - `read_range`（第2段階の仮想化ビューア＝要求位置近傍 1 ウィンドウだけ seek+read し行境界整列して返す・読み取り専用）。
  - `search_in_text`/`replace_in_text`（pika-core::search 委譲・`SearchCancelService` で新検索が前の検索を打ち切る）。
  - frontend 型バインディング（`src/ipc.ts`: openDocument/saveDocument/readRange/searchInText/replaceInText・各 DTO）。
- **本スプリントで系統C（Windows 実機）に残す確認**（acceptance.md L1〜L7）:
  - **L1（design doc 15章-6・必達）CM6 巨大ファイル実測**: 基準機・Release で **10MB のファイルが編集・検索・保存が
    通常通り可能**（第1段階死守）であることを実測。CM6 の編集体感が劣化し始めるサイズを計測し、第2段階を **50MB**・
    上限を **500MB** に確定したこと〔旧 200MB/2GB から Web 現実値へ引下げ〕の妥当性を実機で再確認する。10MB を下回って
    劣化するなら中心体験後退として要件改訂を提案（本スプリントでは 10MB 死守と判断し維持）。
  - L2: Shift_JIS・CRLF のファイルを開いて編集・保存しても、エンコーディングと改行が変わらない（要件5.6 受け入れ基準）。
  - L3: Shift_JIS と誤判定された UTF-8 を「エンコーディングを指定して開き直す（Reopen with Encoding）」で正しく再表示。
  - L4: Shift_JIS で表現できない文字（絵文字等）を入れて保存しようとすると保存が中断し選択肢が提示される（文字欠落なし）。
  - L5: 正規表現＋キャプチャ参照で全置換ができる（実 GUI の検索バー）。
  - L6: 第2段階（50MB 超）の読み取り専用ビューアで閲覧・検索ができ、編集/保存/置換が無効化される。
  - L7: 巨大ファイル/長行での検索・全置換が UI をブロックせず、キャンセルできる（進捗表示・別スレッド実行は実機で確認）。
- **状態（sprint 6・iteration1）**: 決定論ゲート（cargo test pika-core 268 件〔iteration5 比 +61＝huge 13・range 9・
  encoding 22・search 17〕＋pika-cli 11・pika-app 5）PASS・cargo build（crates＋src-tauri・警告エラー扱い・exit 0）／
  npm typecheck 成立。**要件 2.2/5.4/9.2 の TBD を CM6 実測値（第2段階 50MB・上限 500MB・第1段階 10MB 維持）で確定し
  requirements.md を改訂済み（design doc 16章）**。上記 L1〜L7 と実 GUI 反映（仮想化ビューア描画・検索バー・エンコーディング
  メニュー・保存中断ダイアログ）は系統C（Windows 実機）で確認する。

---

## T-010 a11y 全Web再構築・エッジケース・配布（sprint 7・design doc 17章/18章/19章／系統C）

- **重大度**: 中（a11y は初期版の受け入れ基準＝要件11.5・キーボードのみ完走／通知バーキュー運用＝要件11.1／
  非テキスト・FSエッジの縮退でアプリ継続＝要件12.1/12.2／診断ログ＝要件12.3）。データ損失系として保存前の
  incoming 退避漏れ・atomic_write の非アトミック窓は **高**として本スプリントで是正（下記）。
- **対応章**: 要件11.1（通知バーキュー）・11.2（主要ショートカット）・11.5（ARIA/F6/forced-colors）・12.1（FSエッジ）・
  12.2（画像簡易ビュー/寸法プリチェック/非対応バイナリ）・12.3（診断ログ）・13（配布）・design doc 17章/18章/19章・
  ui-design 15章（5状態）。acceptance.md TG1〜TG10。
- **決定論側（cargo test 済み・pika-core・新規 49 件）**:
  - **notify_queue** — `pika-core::notify_queue`。通知バーキュー運用（要件11.1）。`NoticeKind`（優先順位
    **衝突＞設定エラー＞外部リソース＞JS検知＞巨大ファイル制限**・`auto_dismiss`＝衝突だけ閉じるまで残す）・
    `NoticeQueue::push`（**同一ファイル/同一種別を最新へ合体**）・`resolve`（タブ固有はアクティブタブ・グローバルは
    常時・**優先順位→新しさ順で最大3本＋他N件**）・`dismiss`/`dismiss_tab`（自動消滅条件の発火）。要件11.4 の
    「4件以上→3本＋他N件」「同一ファイル衝突は1本」「タブ切替で表示切替」を cargo test で観測（11 件）。
  - **diagnostic** — `pika-core::diagnostic`。診断ログ方針（要件12.3）。`should_log`（**既定 warn 以上**・Info は記録
    しない）・`log_dir`/`current_log_path`（`<data_root>/logs/pika.log`）・`LogLine`（**type に本文フィールドを持たず**
    level/category/op/path/summary のみ＝ユーザー内容を構造的に書けない・`format` は summary 内改行を1行へ畳む）・
    `plan_rotation`（**5MB 超で 3世代ローテーション**＝pika.log→.1→.2・最古削除）。7 件。
  - **nontext** — `pika-core::nontext`。非テキスト/FSエッジ縮退（要件12.1/12.2）。`classify_extension`（画像/テキスト/
    未知は安全側で非対応バイナリ）・`decide_image_open`（**総ピクセル 6000万px 超はデコードせず外部誘導**・寸法不明も
    安全側で外部誘導＝デコード爆発で固まらない）・`degrade_for_edge`（読み取り専用/アクセス権なし/ネットワークドライブ/
    クラウドプレースホルダ/ワークスペース消失の縮退方針を決定論で算定＝機能を縮退してアプリ継続）。10 件。
  - **view_state** — `pika-core::view_state`。ビュー別5状態（ui-design 15章）。`resolve_view_state`（**優先順位
    Error＞Loading＞Partial＞Empty＞Ideal**）・`EmptyReason`（**Empty 3分岐**＝フォルダ未オープン/検索0件/消化後で
    文言が変わる）・`DegradeReason`（Partial の縮退理由＋`can_reenable`＝読み取り専用ビューアのみ再有効化不可）・
    `degrade_reasons`（huge::DegradeFlags から組立）。9 件。
  - **shortcuts** — `pika-core::shortcuts`。主要ショートカット表（要件11.2）。`resolve`（**Ctrl+Enter は差分/プレビュー
    フォーカス時のみ確認済みを発火＝誤爆防止**・Ctrl+Shift+Enter はフォーカス非依存・Ctrl+Alt+Enter は一括・F8系は
    Alt+Down/Up 代替・Ctrl+\\ は Ctrl+Shift+E 代替）。11 件。
- **データ損失系の是正（eval high data 対応・src-tauri）**:
  - **atomic_write の単一アトミック置換化**: 旧実装の Windows フォールバックが `remove_file→rename` の2段で、間で
    クラッシュ/電源断すると元ファイル消失・新ファイル未配置の窓があった。**`MoveFileExW(MOVEFILE_REPLACE_EXISTING |
    MOVEFILE_WRITE_THROUGH)` の1呼び出し**で置換へ変更（`src-tauri/src/document.rs::replace_atomically`）。置換失敗時は
    一時ファイルを後始末し元ファイルは触らない。
  - **保存前の incoming 退避**: `save_document` が破壊的上書きの前に、ディスク上の現内容に未確認の外部変更があれば
    **incoming 退避してから上書き**する（`SnapshotService::stash_incoming_before_overwrite`）。退避が取れなければ保存を
    中断する（退避が先＝CLAUDE.md 判断ガイド・データを失わない）。ベースライン一致/保存内容一致/内容非保存方針は退避不要。
- **配線（cargo build＋npm typecheck 成立）**:
  - 診断ログ: `src-tauri/src/diagnostic.rs`（`record`＝データルート解決→`pika_core::diagnostic` でレベル判定/ローテーション
    計画/整形→FS 追記・`log_folder_path` command＝ログフォルダを作成しパスを返す＝「メニューからログフォルダを開ける」）。
    save/open の失敗経路から `record` を呼び実使用（要件12.3）。
  - frontend: `src/a11y/index.ts`（`initFocusCycling`＝F6/Shift+F6 をペイン間循環・`initLandmarks`＝role/aria-live 確実化）・
    `src/ui/notifications.ts`（NoticeQueue を pika-core 同規則で実装・actions/閉じる/他N件描画・activateTab で setActiveTab）・
    `src/ui/status.ts`（`renderStatus`＝差分あり件数を aria-label 化）・`src/ui/viewstate.ts`（5状態文言・degradeReasonsFromFlags）・
    `src/ui/image.ts`（画像簡易ビュー fit/actual・「既定アプリで開く」誘導）・`src/styles/app.css`（通知 actions・画像/誘導・
    `@media (forced-colors: active)` でトークン降格）・`src/ipc.ts`（logFolderPath）。
- **本スプリントで系統C（Windows 実機）に残す確認**（acceptance.md TG1〜TG10）:
  - TG1: ナレーター/UIA でツリー/タブ/通知/ステータスの aria が辿れる（design doc 15章 Open 4＝プレビュー別WebView 内本文
    の読み上げ可否も実機検証）。TG2: F6/Shift+F6 のペイン循環でキーボードのみ中心体験完走。
  - TG3: 通知4件以上で3本＋他N件・タブ切替で切替・自動消滅。TG4: 5状態遷移と Empty 3分岐・Partial 通知。
  - TG5/TG6: 画像簡易ビュー・寸法プリチェックで巨大画像を外部誘導・非対応バイナリ/FSエッジ縮退でアプリ継続。
  - TG7: 診断ログの実出力（warn 以上・本文非記録・5MB×3世代ローテーション・ログフォルダを開く）。
  - TG8: ショートカットが表どおり発火（誤爆防止/代替割当）。TG9: forced-colors/テキストスケール追従。TG10: bundler 配布。
- **未対応（リリース準備・系統C／別途）**: Tauri bundler の実インストーラー・エクスプローラー統合（HKCU 登録/解除）・
  ジャンプリスト実機表示・About のライセンス同梱・診断ログのメニュー導線の GUI 結線（command は実装済み）。
  `cargo audit` は本環境に未導入（sprint 7 verify は `cargo test`/`cargo build`）。安定実行できる段で CI ゲートに載せる。
- **状態（sprint 7・iteration1）**: 決定論ゲート（cargo test pika-core **317 件**〔T-009 比 +49＝notify_queue 11・
  diagnostic 7・nontext 10・view_state 9・shortcuts 11・他+1〕＋pika-cli 11・pika-app 5）PASS・cargo build（crates＋
  src-tauri・警告エラー扱い・exit 0）／npm typecheck 成立。上記 TG1〜TG10 の実 GUI 反映は系統C（Windows 実機）で確認する。

## T-011 — sprint 7 iteration 2: a11y キーボード操作性／ショートカット配線／保存エンコーディング維持の是正

前ターン eval（turn-7-1）の feedback を high → medium の順に是正した。決定論ゲート（cargo test 317 件）は据え置き
（本是正は frontend 配線とデータ整合の結線が中心で pika-core ロジックは非回帰）、cargo build／npm typecheck は exit 0。

- **[HIGH] ファイルツリーのキーボード操作性（要件11.4/11.5・design doc 17章 must）** — `src/ui/tree.ts` を本実装。
  - **roving tabindex**: ツリー内で常に 1 つの treeitem だけ `tabIndex=0`（残り -1）。Tab 一発でツリーへ入れる。
  - **↑/↓/Home/End** で treeitem 間移動・**Enter/Space でファイルを開く**（openFile 起動）。これでマウスなしで
    「開く→プレビュー→差分→確認済み」の**起点に到達できる**（前ターンは treeitem=tabIndex-1＋click のみで到達不能だった）。
  - **ディレクトリ treeitem に `aria-expanded`** を付与（sprint 7 must の明示要求。現状ツリーは 1 段表示のため折りたたみ
    `false` として表現）。状態マーク（±/◆/取消線）を `aria-label` にテキスト化（色/記号に依存しない読み上げ）。
  - フォーカス可視化を `app.css` の `:focus-visible` で追加（キーボードで入った位置が分かる）。実読み上げは系統C（TG1/TG2）。
- **[HIGH] 主要ショートカット表の frontend 配線（要件11.2）** — `pika-core::shortcuts::resolve`（cargo test 済み）の
  写し `src/shortcuts.ts`（`resolveShortcut`・パリティ契約をヘッダに明記）を新設し、`main.ts` の独自インライン keydown を
  **resolve ベースのディスパッチ**（`currentFocus`→`resolveShortcut`→`dispatchAction`）へ置換。これで前ターン dead code 化
  していた Ctrl+O/Ctrl+Shift+O/Ctrl+S/Ctrl+W/Ctrl+F/Ctrl+H/Ctrl+\\/Ctrl+Shift+E/Ctrl+Shift+D/F8系/Ctrl+Enter 系が
  **キーボードから発火**する。Ctrl+Enter の誤爆防止（差分/プレビューフォーカス時のみ確認済み）も currentFocus で結線。
  検索/置換バー UI 実体は系統C（TG8 で表どおりの発火を実機確認）。
- **[MEDIUM] 保存のエンコーディング維持（要件5.2/5.6・最上位原則）** — `main.ts` の保存導線を旧 `save_file`（UTF-8 直書き）
  から **`save_document`** へ寄せた。開くときも `read_file` ではなく `open_document` を使い**検出エンコーディング/BOM を
  タブに保持**して保存時へ渡す。これで Shift_JIS 等が暗黙 UTF-8 化されず、表現不能文字があれば**保存を中断**して
  ［UTF-8で保存/キャンセル］を提示する（save_document は退避が先＝incoming 退避→失敗なら中断も担保）。
- **[MEDIUM] 診断ログの並行書込直列化（要件12.3）** — `src-tauri/src/diagnostic.rs` の `log_with_min` 全体を
  プロセス内 `OnceLock<Mutex<()>>` で囲み、「サイズ確認→ローテーション→追記」を不可分区間にした。複数スレッド
  （watcher/command/search）が同時に record() しても世代ずれ/行取りこぼし/5MB超過 append が起きない。毒ロックは
  into_inner で続行しアプリを止めない（診断は副次）。`plan_rotation` の純粋ロジック自体は pika-core で既にテスト済み。
- **[MEDIUM] フォルダ切替の未保存確認を三択化（要件5.3）** — `main.ts` `switchFolder` の二択 window.confirm を、
  **対象ファイル名を列挙**したうえで「保存して切替／破棄して切替／キャンセル」の三択へ（window.confirm 2段で表現）。
  「保存して切替」は save_document 経由で全 dirty タブを保存し、保存中断/失敗が残れば切替を中止する（無確認の喪失をしない）。
- **[MEDIUM] ARIA 到達性の補強** — (1) ディレクトリ treeitem に aria-expanded（上記）。(2) 表示専用ステータスとは別に、
  視覚的に隠した polite ライブ領域 `#sr-live`（`.sr-only`）を追加し**差分あり件数を announce**（aria-live=off の div への
  aria-label 依存より読み上げ到達が確実）。(3) `src/ui/tabs.ts` に **tablist の roving tabindex＋←/→/Home/End 移動**を実装し
  バッジ状態を aria-label テキスト化。実読み上げは系統C（TG1）。
- **[LOW] 環境制約の記録** — `cargo audit` は本環境に cargo-audit 未導入で未実行（sprint 7 verify 配列は cargo test/cargo build
  のみ・テスト緩和ではない）。comrak(unsafe_HTML)/ammonia/Mermaid/KaTeX/highlight の攻撃面依存があるためリリース前（系統C）に
  advisory DB 照合結果をここへ記録する。SnapshotService のインメモリ永続化はリリース前に index.json+content-addressed object
  復元が要る（sprint 3 からの繰り越し・本スプリント非回帰）。
- **状態（sprint 7・iteration2）**: cargo test（pika-core 317・pika-cli 11・pika-app 5）PASS・cargo build（警告エラー扱い・
  exit 0）・cargo fmt --check clean・npm typecheck 成立。上記の実 GUI 反映（キーボードのみ中心体験完走・ショートカット発火・
  読み上げ・三択確認）は系統C（TG1/TG2/TG8）で確認する。

---

## Tauri 刷新後 実機検証（系統C・2026-06-21）

> 検証環境（旧 wx 版から更新）: Tauri 版 debug ビルド `target/debug/pika.exe` ＋ Vite 開発サーバ
> （`npm run dev`・devUrl `http://localhost:5173`）。フィクスチャ `C:\Users\nonpr\pika-accept`。
> 画面取得は Python(ctypes) の `PrintWindow(PW_RENDERFULLCONTENT)`（遮蔽非依存・PowerShell 不使用）。
> 起動の落とし穴: **debug ビルドは埋め込み dist でなく devUrl を見にいく**（Vite 未起動だと
> `ERR_CONNECTION_REFUSED`）。出荷資産そのままで見るには release ビルド。

## F-027 保存失敗トーストの文言が二重化（「保存に失敗しました: 保存に失敗: …」）

- **重大度**: 低（軽微・体感。機能は正常で文言のみ冗長）
- **対応章**: 保存導線（要件5・通知）
- **現象**: readonly.md（R 属性）への保存失敗時、トーストが
  `保存に失敗しました: 保存に失敗: アクセスが拒否されました。(os error 5)` と「保存に失敗」が二重で出る。
- **根本原因**: backend が既に接頭辞付きのエラー文字列を返すのに、frontend がさらに接頭辞を前置する二重ラップ。
  - `src-tauri/src/commands.rs:94` … `std::fs::write(...).map_err(|e| format!("保存に失敗: {e}"))`
  - `src-tauri/src/document.rs:244` … `format!("保存に失敗: {e}")`
  - `src/main.ts:448` … `notify(\`保存に失敗しました: ${String(e)}\`, "error")`
- **対応**: 接頭辞をどちらか一方に統一する（frontend は素のエラーをそのまま表示する／または backend は素の OS
  エラーを返し frontend が文脈を付ける、のいずれか）。日本語として自然な 1 文にする。
- **状態**: 発見（コードで確定・保存失敗時は必ず再現）

## F-028 削除済みファイルの取り消し線がアイコン（📄/📁）も横切る

- **重大度**: 低（軽微・体感。ui-design 5章の状態記号表現の精度）
- **対応章**: ツリー/タブの状態記号（ui-design 5章・要件7.2/11.5）
- **現象**: 削除済みタブをツリー/タブに残すとき、取り消し線が**行全体**に掛かり、ファイルタイプアイコンや
  タブの × 閉じボタンまで横切る。取り消し線はファイル名テキストのみに掛けるのが自然。
- **根本原因**: アイコンと名前を同一テキストノードに入れ、行要素全体へ line-through を適用している。
  - `src/ui/tree.ts:48-54` … `li.textContent = \`${icon} ${entry.name}${suffix}\`` とした `<li>` 全体に
    `li.style.textDecoration = "line-through"`。
  - `src/ui/tabs.ts:39` … タブボタン全体に line-through（× 閉じボタンも対象）。
- **対応**: ファイル名（とサフィックス）を専用 `<span>` に分離し、その span にのみ line-through を適用。
  アイコン/× は装飾対象外にする。aria-label による読み上げ表現（要件11.5）は現状維持。
- **状態**: 発見（コードで確定・削除済み表示時に必ず再現）

## 非再現として close した所見（2026-06-21 トリアージ）

実機初回キャプチャで観測した以下 2 件は、退避→クリーン起動→state.json 再注入の切り分けで**バグでないと確定**。
記録のみ残す（再オープン／起動時復元のいずれでも再現せず）。

- **ambiguous.txt の削除済み（取り消し線）表示**: 新規オープン=通常表示／起動時復元=`±`（差分あり＝保存時
  ハッシュとディスク内容が異なるための**正しい**未読判定）／CLI 再オープン=無印。一度だけ見えた取り消し線は
  過去セッションの残存/過渡状態。コアの復元3分岐（Missing→Deleted／Changed→Unread／Same→Restore・
  `pika-core::state::restore_tab`）はロジック上正しい。
- **起動/復元時の保存失敗トースト**: 起動時復元（`restoreOnStartup` は `saveOnce` を呼ばない）でも CLI 再オープン
  でも未再現。readonly.md（R 属性）への保存が `os error 5` で失敗するのは**正しい挙動**。残る問題は文言のみで
  F-027 に集約。

## T-012 プレビュー実結線 Stage ①（子WebView オーバーレイ最小貫通／系統C）

中心体験の核「プレビュー」がライブUIで全く描画されない問題（別WebView 未生成・未ナビゲート）を、
権限ゼロの子WebView オーバーレイ（Tauri v2 `Window::add_child`・`unstable` feature）で実結線した。
引き継ぎ `docs/handoff-preview-wiring.md` の段階分割①を完了。

- **構成**: `setup()` では子WebView を作らず、イベントループ稼働後の `RunEvent::Ready` で
  `preview::create_preview_webview` を呼ぶ（**setup 内同期 `add_child` はイベントループ未稼働で
  デッドロック**＝メイン窓が不可視のまま約35MB で固着する回帰を実機特定→修正）。
- **配信**: custom protocol `/doc/<gen>` を pika-core の純粋関数 `wrap_preview_document` で
  `<!DOCTYPE>`/`<head>`（charset・最小 base CSS）付き完全文書にラップして返す（body 不変・CSP ヘッダ強制・
  `<meta>` CSP 非依存）。HTML はメインWebView の JS ワールドを経由しない。
- **frontend**: `renderActivePreview` が `#preview-host` の矩形＋URL を `show_preview` へ、占有解除で
  `hide_preview`、`ResizeObserver`＋window resize で `set_preview_bounds`。
- **実機検証（debug＋Vite・showcase.md）**:
  - ✅ メイン窓が可視化（デッドロック解消）。
  - ✅ プレビュー子WebView が `#preview-host` 領域にオーバーレイ表示。H1／GFM テーブル／タスクリスト
    （チェックボックス）／段落が base CSS 付きで描画。日本語も charset 正常。
  - ✅ トグルオン=show、トグルオフ=hide でエディタへ復帰。
  - ✅ ウィンドウ縮小に子WebView が追従（`set_preview_bounds`）。リフローし右端の覗きも解消。
  - 想定どおり未描画（Stage ② 対象）: Mermaid（`flowchart LR …` 生表示）／KaTeX（`$…$` 生表示）／
    コードハイライト。アセット配信・信頼 JS 注入は未実施。
- **軽微所見（後段で詰める）**: 既定窓幅では子WebView 右端が `#preview-host` よりわずかに内側で、
  下のエディタが ~20px 覗く瞬間がある（DPI/領域追従の微差・Stage ③）。
- **繰り越し（必須・未実施）**:
  - 権限ゼロ実証（プレビューWebView から `window.__TAURI_INTERNALS__` 不在・任意 invoke 失敗）の
    **JS 実行による形式検証**。アーキ上は「capability 不在＋remote オリジン（`pika-preview.localhost`）で
    app command 自動拒否」で担保されるが、実機の到達不能確認は別途行う。
  - Stage ②（Mermaid/KaTeX/highlight アセット配信＋信頼 JS 注入）／Stage ③（テーマ CSS 変数・
    失敗件数の Tauri event 化・占有[プレビュー＋差分]の左右並置）／Stage ④（HTML 系統B・外部 opt-in・
    暴走ガード実機）。
- **状態**: Stage ① 完了（4ゲート緑＋実機目視 OK）。
