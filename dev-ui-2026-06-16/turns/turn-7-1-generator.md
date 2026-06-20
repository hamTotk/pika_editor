# turn-7-1 generator report — sprint 7「周辺機能の肉付け」

## スプリント目標（再注入）

検索/置換 UI（core/search 配線）、状態復元の組み立て（state.json→AppState→タブ/ツリー展開/モード/差分/
ペイン収納の再構成）、settings.toml 監視反映（core/settings 配線・読み取り専用）、最近使った項目/ジャンプ
リスト、通知バー集約 ViewModel（最大3本+他N件・優先順位・集約）を実装し、controller/ViewModel 部分を
決定論テストする（spec.md 系統A）。

本フェーズの検証戦略（spec.md）どおり、wx 非依存に切り出せるアプリ層ロジックを `src/controller/` に実装し
`pika_core`（PIKA_BUILD_GUI 非依存の静的lib）に含めて `pika_tests`（gtest）で決定論検証＝系統A（must）、
GUI 配線のコンパイル+リンク成立＝系統B（must）とした。検索パネル描画・設定監視 I/O・状態復元の実結線は
GUI 実機が要るため系統B/C へ寄せる（spec.md「非対象／本フェーズの自動 verify に載せない」に整合）。

## 変更ファイル一覧

### 新規（controller・wx 非依存ロジック。pika_core に含む）

- `src/controller/restore_plan.h` / `.cpp` — 状態復元の組み立て（AppState→RestorePlan）
- `src/controller/notification_model.h` / `.cpp` — 通知バー集約 ViewModel
- `src/controller/search_session.h` / `.cpp` — 検索/置換結線（SearchEngine ラッパ＋カーソル遷移）
- `src/controller/settings_view.h` / `.cpp` — settings.toml 反映の純粋写像

### 新規（gtest・pika_tests に含む）

- `tests/controller/restore_plan_test.cpp`（18 ケース）
- `tests/controller/notification_model_test.cpp`（11 ケース）
- `tests/controller/search_session_test.cpp`（14 ケース）
- `tests/controller/settings_view_test.cpp`（7 ケース）

### 変更（ビルド構成）

- `src/CMakeLists.txt` — 上記 4 controller モジュールを `pika_core` のソースに追加＋sprint7 の意図コメント
- `tests/CMakeLists.txt` — 上記 4 テストを `pika_tests` に追加

注: `dev/state.json` の差分は run-dev ハーネス自身のループ状態であり generator は触っていない。

## must criteria ごとの実装状況（すべて自動テスト化済み）

### must#1 状態復元の組み立て（restore_plan）— 充足

`build_restore_plan(AppState)` を純粋関数として実装。
- 欠落フィールドは安全な既定値で補完: `diff_on=false`・`tree_pane_collapsed=false`
  （`tree_pane_collapsed` は core::state::AppState に未保持のため controller 層で常に既定 false 補完）。
- 未知 version（`> kStateVersion`）は `restorable=false`＝復元しない（中身も読まない）。core/state の
  `load_state` も Unsupported で弾く二重ガード。「書き戻さない」は本層が読み取り専用写像で書込経路を持た
  ないことで担保（design 7章 K2）。
- 旧 4モードの `mode` 文字列を 3モード＋差分トグルへ正規化（`normalize_mode`：`"diff"`→
  `(Source, diff_on=true)`／不明・空→`(Source, false)`。ui-design 14章のモデル変更）。
- active_tab のタブ範囲クランプ・ウィンドウジオメトリの有効判定（width/height>0）・theme 文字列の
  ThemeKind 写像・recent の最大20件クランプ（core::state::kRecentLimit を再利用＝足さない）も観測。
- テスト: 未知version非復元・モード正規化・既定補完・クランプ・テーマ・recent・決定論を 18 ケースで観測。

### must#2 通知バー集約 ViewModel（notification_model）— 充足

`aggregate_notifications(notifications, active_tab_path)` を純粋関数として実装（design 10章 J1）。
- 最大 3 本（`kMaxVisible`）＋「他N件」（`overflow`）集約。
- 優先順位（高い順）: 衝突 ＞ 設定エラー ＞ 外部リソース参照 ＞ JS検知 ＞ 巨大ファイル（NotificationKind の
  列挙順）。同種別内は seq 降順（新しいもの優先）。
- 同一ファイル・同一種別は最新（seq 最大）の 1 件へ集約（キー＝(kind, tab_path)）。
- タブ固有/グローバルの切替: グローバル（tab_path 空）＋アクティブタブ固有のみ対象、他タブ固有は除外。
- テスト: 優先順位・衝突最優先・同種最新集約・同一ファイル別種別非集約・他タブ除外・グローバル常時表示・
  overflow 境界（3本ちょうど/超過）・空入力・決定論を 11 ケースで観測。

### must#3 検索/置換ロジックの結線（search_session）— 充足

`SearchSession`（core/search::SearchEngine の薄いラッパ）＋カーソル遷移純粋関数を実装（要件5.4）。
- `find_all`/`replace_all` を SearchEngine へ委譲し Result/truncated/cancelled をそのまま透過（握り潰さ
  ない）。リテラル・正規表現（キャプチャ参照 $1）・不正正規表現の InvalidArgument・協調キャンセルを観測。
- `next_match`/`prev_match`: キャレット位置（UTF-8 バイトオフセット）から次/前のヒットへ遷移、末尾↔先頭の
  折り返し（wrapped）、ヒット0件の found=false を決定論で解く。
- テスト: find_all/replace_all 結線（件数・置換結果・無効正規表現・キャンセル透過）＋カーソル遷移（前後・
  caret 同位置スキップ・折り返し・空）＋決定論を 14 ケースで観測。

### must#4 settings.toml 監視反映（settings_view）— 充足

`apply_settings(LoadResult)`＋`to_ui_settings`/`to_view_mode`/`to_theme_kind` を純粋写像として実装。
- core::settings::Settings の enum（ViewMode 4モード・Theme）を controller 語彙（ViewMode 3モード・
  ThemeKind）へ写し替え（Diff モードは差分トグル側のため Source へ畳む。ui-design 14章）。
- parse_ok=false（構文破損＝保存途中の不完全 TOML 等）は `apply=false`＝直前の有効値を維持し再適用しない
  （ちらつかせない。要件10.3）。
- 不正値の warnings 件数を `warning_count` として UI へ伝える（通知バー SettingsError 用）。
- pika は settings.toml に書き戻さない（読み取り専用）＝入力 LoadResult を変更しない純粋写像（テストで
  入力不変を観測）。
- テスト: enum 写し替え・フィールド写し・正常反映・不正値の warnings 件数・構文破損の非再適用・入力不変
  を 7 ケースで観測。

### must#5 ビルド（系統A/B verify）— 充足

- `ctest --preset x64-core-test`: 597 tests PASS（従来 547 ＋ 新規 50 件。非回帰）。
- `cmake --build --preset x64-release`: コンパイル＋リンク成功（exit 0。/W4・警告エラー扱いで無警告）。
  新規 controller は pika_core 経由で pika.exe／pika.com／pika_tests の全ターゲットにリンク済み。

## should criteria の状況

- 最近使った項目（最大20件）の保持・引数なし起動での完全復元: 復元素材（recent_files/folders の20件
  クランプ・last_workspace・消失タブの扱いに使う tabs.path）は RestorePlan に含めテスト済み。GUI への
  ジャンプリスト更新・実 wxFrame 復元結線は系統B/C（GUI 実機）へ繰延（spec.md の検証戦略どおり）。
- テーマ解決の現在値を state.json に保持し wxSysColourChangedEvent で再適用: テーマ現在値の写像
  （normalize_theme／to_theme_kind）は実装・テスト済み。wxSysColourChangedEvent ハンドラ（既存 main_frame
  に骨格あり）への実反映は系統B/C へ繰延。
- 巨大ファイル段階制（10MB/200MB）・第2段階の置換UI無効化を DocState フラグ参照で分散させず判定:
  閾値（big_file_stage1/2_bytes）を UiSettings 経由で一元化する素材を用意。判定の DocState 配線は GUI 側で
  既存 diff_mode_model（差分自動オフ）と同様に行う系統B 範囲のため本ターンでは決定論部分の素材整備に留めた。

## テスト化できなかった criteria とその理由

なし（must 4 件すべて wx 非依存の純粋ロジックとして gtest 化）。should の GUI 実結線（ジャンプリスト・
wxSysColourChangedEvent 実反映・置換UIの実無効化）は wx/Win32 実機が要りユニットテスト不能のため、
spec.md の検証戦略（系統B コンパイル成立＋系統C 手動 acceptance）に従い繰延。テストの緩和・スキップ・
無効化は行っていない（全 50 ケースが実挙動を観測。犠牲的期待値なし）。

## 自己実行した verify の結果

- `ctest --preset x64-core-test` → 100% tests passed（pika_tests_build＋pika_tests の 2/2）。
  内訳: pika_tests バイナリで 597 tests / 80 suites が PASS（新規 50 件含む）。
- `cmake --build --preset x64-release` → exit 0（コンパイル＋リンク成功・/W4 無警告）。

（合否判定の正本は run-dev が別途実行する結果。上記は自己確認。）

DONE: C:/dev/pika_editor/dev/turns/turn-7-1-generator.md
