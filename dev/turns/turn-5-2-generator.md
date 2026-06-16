# turn 5-2 generator report — sprint 5（中心体験③『差分』）iteration 2

## 概要

iteration 1（score 86・fail）の eval は d_verify=100 を通したが **d_accept=0**（must#4
「JS 有効/無効の直列切替」が未配線）で落ちた。本 iteration は `feedback_structured` の high →
medium → low の順に対応し、must#4 を実コードで成立させた。verify 二系統（ctest core-test ＋
release ビルド）はともに exit 0。

## 変更ファイル一覧

- `src/controller/preview_builder.h` / `.cpp`（pika_core・wx 非依存・gtest 対象）
  - `classify_preview_kind(name_or_path)`：拡張子→`PreviewKind`（.html/.htm=Html=JS無効・他=Markdown=JS有効）。must#4 の JS 切替の判定根拠を純粋関数化。
  - `is_diffable_type(name_or_path)`：.md/.markdown/.html/.htm/.svg のみ差分対象（`DiffDisableReason::NotDiffableType` の入力）。
  - `build_diff_body`：`DiffLine::spans`（行内強調）を `<span class="diff-word">` でラップして出力（行内チャンネル＝色＋記号＋行内下線の多重符号化。eval medium）。範囲外/逆転 span は堅牢に無視。
  - `build_head`：`<base href="https://doc.pika/">` を**廃止**。CSP `base-uri 'none'` で無効化されるため矛盾を解消し、相対解決は「ページ URL = https://doc.pika/ からのナビゲート（SetPage の baseUrl）」へ一本化（eval medium「base 方針の統一」）。
- `src/ui/preview_view.h` / `.cpp`（wx 依存・系統B）
  - **JS 有効/無効の実切替**：`apply_script_enabled(bool)` が `wxWebView::GetNativeBackend()`（Edge=`ICoreWebView2*`）→`get_Settings()`→`ICoreWebView2Settings::put_IsScriptEnabled(BOOL)` を呼ぶ。`navigate()` がナビゲート**前**に kind から JS 設定を適用（eval high・design 6章 C5 の二層目防御）。
  - **直列化**：`nav_in_flight_`（SetPage→on_loaded 間）と `pending_`（最新 1 件のみ保持）。in-flight 中の要求は保留し `on_loaded()` で流す＝前モード JS 設定の残留・切替競合を防ぐ（design 6章 C5）。
  - **app.pika 仮想ホスト配線**：`AppPikaHandler` を登録（iter1 は doc.pika のみで片肺だった。eval high）。exe 隣 `assets/` から `https://app.pika/preview.css` 等の同梱信頼アセットを配信。
  - **doc.pika ハードニング**（eval medium）：(1) `url_decode_once`（%2e%2e%2f 等のエンコード済みトラバーサルを正規化前に 1 回展開）→(2) `normalize_to_absolute` で `..` 畳み込み→(3) `under_root` 必須→(4) **許可リスト方式**の拡張子→Content-Type（`content_type_for`）。app.pika 側も同じ ../ 遮断・許可リストを適用。
  - **二相 API**（`request_occupy`/`apply_document`）：ワーカーオフロード用。要求時点で占有して stamp を確定、完了後 `is_current(stamp,key)` が真のときだけナビゲート（中間状態を描かない。design 5.5 手順3）。
  - 実ファイル read 失敗は `FinishWithError`（白紙ではなく欠落）。
- `src/ui/main_frame.h` / `.cpp`（wx 依存・系統B）
  - **差分トグル無効化の GUI 結線**（eval high）：`update_diff_toggle_state()` が `evaluate_diff_toggle(ctx)` を呼び、`ID_TOGGLE_DIFF` の `Enable/Check` と理由文言（`diff_disable_reason_label` 経由・左ステータス）へ反映。無効時は `diff_on_=false` を強制＝**無言の空差分を出さない**（ui-design 15章 Partial）。`update_preview()` 冒頭・タブ切替（`on_notebook_page_changed`）で再評価。
  - **JS 切替の到達経路**：`update_preview()` が `active_file_path()` から `classify_preview_kind` で `PreviewKind` を決め、`apply_document(..., js_kind)` へ渡す（iter1 は doc.kind を常に Markdown 固定で HTML プレビュー経路が未到達だった）。差分面は信頼 HTML＝Markdown 相当、プレビューは種別どおり。
  - **TaskRunner ワーカー化**（eval high・performance）：`render_markdown` と `DiffEngine::compute` を `util::TaskRunner::submit` でワーカー実行し UI を 200ms 以上ブロックしない。完了後 `CallAfter`→`apply_document`（占有世代照合）。
  - **empty/error フォールバック**（eval medium）：本文生成失敗時に白紙ではなく `<p class="pika-placeholder">プレビューを生成できませんでした</p>`。
  - **外部リンクのスキーム許可リスト**（eval low）：`on_preview_navigate` は http/https/mailto のみ `wxLaunchDefaultBrowser` へ委譲（file:/UNC/カスタムプロトコルの自動起動を防ぐ多層防御）。
  - `asset_dir()`：`wxStandardPaths` で exe 隣 `assets/` を解決し PreviewView へ供給。
- `src/app/CMakeLists.txt`：`pika` の POST_BUILD で `assets/`→exe 隣 `assets/` を copy_directory（app.pika が実描画でスタイルを当てられるよう同期。系統C 用）。
- `assets/preview.css`（新規）：ui-design 2章カラートークン（diff-add/del-{bg,tx,word}・accent）を CSS 変数化、`prefers-color-scheme` でライト/ダーク追従、差分の行背景＋行内下線（`diff-word`）＋grid レイアウト＋placeholder。`https://app.pika/preview.css` から配信（eval low「preview.css 不在」を解消）。
- `tests/controller/preview_builder_test.cpp`：新規テスト 6 件追加・CSP テスト 1 件更新。

## must criteria の実装状況（sprint 5）

| # | criteria | 状況 |
|---|---|---|
| 1 | 差分モード状態機械（直交組合せ・描画面構成・占有世代） | 充足（iter1 で実装・テスト済み。`diff_mode_model` の 6 組合せ・占有世代を非回帰で PASS） |
| 2 | 差分トグルの自動無効化（10MB/WebView2不在/ベースライン未取得・理由） | 充足（純粋ロジック gtest 済み＋**本 iter で GUI 結線**＝Enable/Check/理由表示） |
| 3 | 共有1枚 WebView2・unified 差分HTML（色非依存 +/-・再ナビゲート・占有世代再照合） | 充足（**本 iter で行内 span も HTML 出力**・TaskRunner 経由で再ナビゲート・stamp 照合） |
| 4 | 仮想ホスト app.pika/doc.pika・CSP・**JS有効/無効の直列切替** | **本 iter で充足**：app.pika ハンドラ登録＋preview.css 配信、JS は `put_IsScriptEnabled` をナビゲート前に適用・on_loaded 直列化、HTML プレビュー経路（`classify_preview_kind`）到達、doc.pika は URL デコード＋..遮断＋許可リスト拡張子 |
| 5 | release ビルド成功（/W4 exit 0）＋ctest core-test PASS | 充足（両 verify exit 0） |

### must の自動テスト化状況

- must#1/#2/#3 の判断ロジック（`resolve_pane_layout`・`evaluate_diff_toggle`・占有世代・`build_diff_body`・`classify_preview_kind`・`is_diffable_type`・行内 span・CSP）は wx 非依存純粋関数として gtest 化（`diff_mode_model_test`・`preview_builder_test`）。
- **テスト化できなかった must#4 の一部とその理由**：`put_IsScriptEnabled` の実適用・直列ナビゲートの順序保証・app.pika/doc.pika の実描画・実ファイル配信は **wxWebView/WebView2 実機（Edge ランタイム＋メッセージループ）が必須**でユニットテスト不能。spec.md「検証戦略 系統B（コンパイル＋リンク成立）＋系統C（手動・acceptance.md）」の設計どおり、判定可能部分は純粋関数へ切り出して gtest 化し、実配線はビルド成立＋手動へ寄せた。JS 切替の**判定根拠**（kind 分類）はテスト化済みで、実適用箇所（`apply_script_enabled`）は系統B で配線成立を担保。

## should criteria

| criteria | 状況 |
|---|---|
| TrySuspend アイドル回収（Resume 300ms は手動） | 状態保持のみ（iter1 同様。実 TrySuspend は wxWebView 非公開のため系統C） |
| ナビゲーションインターセプト・スクロール同期・プレビュー内検索 | インターセプトは配線済み。スクロール同期/Find は系統C（実描画必須）で未配線＝report 明示 |
| 差分計算を TaskRunner で UI を 200ms 以上ブロックしない | **本 iter で充足**（`tasks_.submit` で render+diff をワーカー化・占有世代照合で結果適用） |

## eval feedback への対応（high→medium→low）

- high「JS 切替未配線」→ `apply_script_enabled`＋直列化＋`classify_preview_kind` で実配線（must#4 主因を解消）。
- high「performance/UIブロック」→ TaskRunner オフロード＋占有世代照合。
- high「差分トグル無効化の GUI 未結線」→ `update_diff_toggle_state` で Enable/Check/理由表示・強制オフ。
- high「app.pika 未登録」→ `AppPikaHandler` 登録＋preview.css 配信＋POST_BUILD コピー。
- medium「CSP/base 矛盾」→ `<base>` 廃止し SetPage baseUrl へ一本化（テストも追従）。
- medium「パストラバーサル前ガード」→ 1 回 URL デコード＋..遮断＋許可リスト拡張子（doc/app 両方）。
- medium「行内強調未出力」→ `build_diff_body` が span を `diff-word` でラップ（テスト 3 件追加）。
- medium「empty/error 白紙」→ placeholder 本文。
- low「外部リンクスキーム検証」→ http/https/mailto 許可リスト。
- low「preview.css 不在」→ `assets/preview.css` 新規（ui-design 2章トークン・色非依存）。

## 残課題（スコープ外・後続/系統C）

- ベースライン供給（`has_baseline`）は core/snapshot 結線（sprint6）。現状 `has_baseline=false` 固定のため差分トグルは恒常無効（理由提示つき）＝設計どおりの段階配線。
- `DiffEngine::compute` の `CancelToken` 協調キャンセルは未配線（ワーカーは短命・占有世代で stale 破棄するため実害なし。core 側の開始前サイズ/編集距離ガードは既存利用）。spans の実描画・スクロール同期・TrySuspend 実体・JS 切替順序の実機確認は系統C（acceptance.md・sprint8 で整備）。

## 自己実行した verify の結果（合否の正本は run-dev が別途実行）

- `ctest --preset x64-core-test` → **Passed**（pika_tests_build + pika_tests、100% / 0 failed）。gtest 全 525 件 PASS（iter1 391 → sprint 進行＋本 iter の +6 で増加。非回帰）。
- `cmake --build --preset x64-release` → **exit 0**（pika_core / pika_ui / pika.exe / pika.com / pika_tests を /W4・/WX でコンパイル＋リンク成功。警告ゼロ）。POST_BUILD で `assets/preview.css` を exe 隣へ配置確認。

DONE: C:/dev/pika_editor/dev/turns/turn-5-2-generator.md
