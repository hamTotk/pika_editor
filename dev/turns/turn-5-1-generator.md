# turn-5-1 generator report — sprint 5『差分』（差分モード状態機械＋WebView2 配線）

## サマリ

中心体験③『差分』のアプリケーション層（差分モード状態機械）を controller に決定論実装し、
共有 1 枚 WebView2 とプレビュー/差分 HTML 描画・仮想ホスト・CSP・JS 有効無効の直列切替を
`src/ui` へ配線した。両 verify ゲートとも自己実行で exit 0。

- 系統A（must verify）: `ctest --preset x64-core-test` → **exit 0**（520 件 PASS。前回 493＋新規 27、非回帰）
- 系統B（must verify）: `cmake --build --preset x64-release` → **exit 0**（pika.exe コンパイル＋リンク成功）

## 変更ファイル一覧

### 新規（系統A：wx 非依存・pika_core に含め gtest 対象）
- `src/controller/diff_mode_model.h` / `.cpp` — 差分モード状態機械
  - `resolve_pane_layout(mode, diff_on)`: (ソース/分割/プレビュー) × (差分ON/OFF) の直交→描画面構成
  - `evaluate_diff_toggle(ctx)` / `diff_toggle_enabled` / `diff_disable_reason_label`: 差分トグル
    自動無効化（種別/WebView2/ベースライン/サイズの優先順位つき理由）
  - `OccupancyTracker` / `OccupancyKey`: 占有世代（タブ, モード, 差分ON）の算定と適用前照合
- `src/controller/preview_builder.h` / `.cpp` — プレビュー/差分の完全HTML文書組み立て
  - `build_head`（CSP メタ＋base href doc.pika）, `escape_html`, `build_diff_body`（色非依存 +/-）,
    `build_preview_document` / `build_diff_document` / `build_preview_diff_grid_document`
- `tests/controller/diff_mode_model_test.cpp` — 17 ケース
- `tests/controller/preview_builder_test.cpp` — 10 ケース

### 新規（系統B：wx/WebView2 依存・pika_ui）
- `src/ui/preview_view.h` / `.cpp` — 共有 1 枚 wxWebView（Edge）ファサード
  - 遅延生成、doc.pika カスタムリソースハンドラ（`DocPikaHandler`・親フォルダ配下のみ・`..` 遮断）、
    ナビゲーション全インターセプト、JS 有効無効の直列切替状態、`show_preview`/`show_diff`/
    `show_preview_diff_grid`、占有世代照合、TrySuspend 状態

### 変更
- `src/CMakeLists.txt` — diff_mode_model・preview_builder を pika_core へ追加（sprint5 注記）
- `src/app/CMakeLists.txt` — preview_view を pika_ui へ追加、`wx::webview` リンク、
  `unofficial::webview2::webview2`（WebView2LoaderStatic.lib をフルパス解決＝リンク不全の解消）
- `tests/CMakeLists.txt` — 新規テスト 2 本を追加
- `src/ui/main_frame.h` / `.cpp` — 「表示」メニュー（モード排他ラジオ＋差分チェック）、
  PreviewView を分割レイアウトへ結線、`update_preview()`（diff_mode_model→render_markdown→
  DiffEngine→preview_builder→PreviewView 再ナビゲート）、`on_preview_navigate`、doc_root 更新
- `src/ui/editor_panel.h` / `.cpp` — `text_utf8()`（プレビュー/差分ソース取得）追加
- `src/ui/ui_messages.h` / `.cpp` — モード/差分メニュー文言 ID（単一メッセージ定義経由）

> `dev/state.json` の差分は run-dev ハーネスの実行副産物（自分は編集していない）。

## must criteria 実装状況

| # | criteria | 状況 | 検証 |
|---|----------|------|------|
| 1 | 差分モード状態機械（直交組合せ・描画面構成・占有世代算定） | 実装済 | `DiffModeModelTest`（直交6ケース＋占有3ケース。gtest 観測） |
| 2 | 差分トグルの自動無効化（10MB超/WebView2不在/ベースライン未取得・理由） | 実装済 | `DiffModeModelTest`（無効化5ケース＋境界＋優先順位＋ラベル。gtest 観測） |
| 3 | 共有1枚WebView2に render_markdown／DiffResult を PreviewBuilder 経由で unified 差分HTML（+/-記号・色非依存）。切替で再ナビゲート＋占有世代再照合 | 実装済 | preview_builder=gtest（色非依存 +/-・エスケープ・grid）／WebView2 配線=系統B（ビルド成立）。再ナビゲート/占有照合は `update_preview`＋`PreviewView::occupy`／実描画は系統C |
| 4 | 仮想ホスト app.pika/doc.pika・CSP テンプレート・JS 有効無効の直列切替 | 実装済 | CSP=`build_head` が `build_csp` を適用しテストで script-src/base-uri/form-action/frame-ancestors を観測。doc.pika 配下検証＝`DocPikaHandler::under_root`＋`normalize_to_absolute` の `..` 畳み込み。JS 切替/順序保証の実挙動は系統C |
| 5 | `cmake --build --preset x64-release` 成功＋`ctest --preset x64-core-test` PASS | 達成 | 自己実行で両者 exit 0 |

## should criteria

- TrySuspend アイドル回収: `PreviewView::suspend_if_idle()` で状態を保持、Resume は次回ナビゲートが
  兼ねる骨格を配線（TrySuspend/破棄再生成の実体・300ms 計測は系統C＝acceptance.md）。
- ナビゲーションインターセプト: `wxEVT_WEBVIEW_NAVIGATING` を全 Veto し `on_navigate_` へ振り分け配線済。
  スクロール同期・プレビュー内検索は本ターン未着手（系統C 寄りの実挙動。後続で精緻化）。
- 差分計算/プレビュー変換の TaskRunner（ワーカー）実行: 本ターンは UI スレッド同期呼び（骨格成立優先）。
  ワーカー化＋200ms 非ブロックは後続で結線（占有世代は既にワーカー結果照合に使える形で用意済）。
  差分は core 既実装の開始前サイズ/編集距離ガードを `DiffEngine` がそのまま通す。

## テスト化できなかった criteria とその理由

- **WebView2 実描画・JS 有効無効切替の順序保証・仮想ホスト実配信・再ナビゲートの実挙動**:
  wx/WebView2/実 OS が要りユニットテスト不能（spec.md 系統B/C・design 13章「UI 自動テストは初期版で持たない」）。
  本ターンは判断ロジックを controller（preview_builder の HTML 組み立て・diff_mode_model の状態機械・
  DocPikaHandler の配下検証関数）へ最大限切り出して gtest 化し、残る実配線はビルド成立（系統B）で担保した。
  視覚・実挙動は最終スプリント（sprint8）で `docs/acceptance.md` へ集約する。
- **doc.pika の実ファイル配信**: ハンドラの配下検証ロジックは実装したが、実 FS 配信は系統C。
  配線時点は配下検証成立で空応答を返す（露出面を作らない安全側）。

## 自己実行した verify の結果

```
ctest --preset x64-core-test        → exit 0（pika_tests_build PASS / pika_tests PASS。520/520）
cmake --build --preset x64-release  → exit 0（pika_core / pika_ui / pika.exe / pika.com / pika_tests）
```

新規テスト内訳: `DiffModeModelTest` 17、`PreviewBuilderTest` 10（計 27）。全 PASS、非回帰。

## 補足（評価への申し送り）

- WebView2 リンクは `unofficial::webview2::webview2`（vcpkg の webview2 ポートが提供する
  WebView2LoaderStatic.lib）をフルパスで結線して解消した。wxWidgets の config が bare 名で
  参照しリンカ検索パスに載らない既知の落とし穴で、テスト緩和ではなくビルド成立のための正規解決。
- preview_builder は controller 層に置いた（spec の「PreviewBuilder 配線」）。HTML 組み立ては純文字列
  ロジックで wx 非依存のため、決定論ゲート（系統A）に乗せて security 観点（CSP・エスケープ）を観測可能にした。

DONE: C:/dev/pika_editor/dev/turns/turn-5-1-generator.md
