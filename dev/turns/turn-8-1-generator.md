# turn-8-1 generator report — sprint 8「エッジケース・画像簡易ビュー・GUI 統合と手動 acceptance」

## スプリント目標（再注入）

ラスター画像の wxImage 簡易ビュー（WebView2 不使用・ピクセル数ガード）、読み取り専用/権限/ネットワーク
ドライブ/クラウドプレースホルダの縮退表示、診断ログ（内容を書かない）を配線し、docs/acceptance.md に
視覚・実挙動の手動チェックリストを整備する（系統C）。

本フェーズの検証戦略（spec.md）どおり、wx 非依存に切り出せるアプリ層ロジック（縮退判定・ビュー5状態の
解決）を `src/controller/` に実装し `pika_core`（PIKA_BUILD_GUI 非依存の静的lib）に含めて `pika_tests`
（gtest）で決定論検証＝系統A（must）、GUI 配線のコンパイル+リンク成立＝系統B（must）とした。画像簡易
ビュー実描画・診断ログ I/O・実 FS 属性での挙動は GUI 実機が要るため docs/acceptance.md（系統C）へ集約した
（spec.md「非対象／本フェーズの自動 verify に載せない」に整合）。これは本フェーズ最終スプリントであり、
sprint8 must#3 の「acceptance.md 整備」自体が系統C のフェーズ集約点である。

## 変更ファイル一覧

### 新規（controller・wx 非依存ロジック。pika_core に含む）

- `src/controller/degrade_model.h` / `.cpp` — エッジケースの縮退判定（縮退種別＋次の一手・優先順位解決）
- `src/controller/view_state.h` / `.cpp` — ビュー別5状態（Ideal/Empty/Loading/Partial/Error）解決 ViewModel

### 新規（gtest・pika_tests に含む）

- `tests/controller/degrade_model_test.cpp`（19 ケース）
- `tests/controller/view_state_test.cpp`（17 ケース）

### 新規（手動 acceptance・系統C のフェーズ集約点）

- `docs/acceptance.md` — 系統C 手動チェックリスト（A〜K 群・各項目に requirements/design/ui-design 節を併記）

### 変更（ビルド構成）

- `src/CMakeLists.txt` — degrade_model・view_state を `pika_core` のソースに追加＋sprint8 の意図コメント
- `tests/CMakeLists.txt` — degrade_model_test・view_state_test を `pika_tests` に追加

注: `dev/state.json`・`dev/turns/*-ctx.json` の差分は run-dev ハーネス自身のループ状態であり generator は触っていない。

## must criteria ごとの実装状況

### must#1 縮退判定ロジック（degrade_model）— 充足（自動テスト化済み）

`resolve_degrade(DegradeInput)` を純粋関数として実装（要件12.1/12.2・2.2・design 10章 B3）。
- 6 ケースを縮退種別 `DegradeKind` ＋次の一手 `NextStep` ＋継続性 `can_continue`／読込ブロック
  `blocks_content` へ写像: 読み取り専用（ReadOnly→SaveAsOrUnlock）・権限なし/排他ロック
  （AccessDenied→RetryOrClose・読込ブロック）・シンボリックリンク循環（SymlinkLoop→枝打ち切り）・
  ネットワークドライブ（NetworkDrive→PollingNotice・内容は読める）・クラウドプレースホルダ
  （CloudPlaceholder→OpenOnDemand・列挙時は内容読込せずハイドレーション抑止）・画像ピクセル数超過
  （ImageTooLarge→OpenInDefaultApp・デコードせず）。
- 画像ガードは `is_image && pixel_count > max_pixels`（境界は `>` のみ＝ちょうど上限はデコード可）。
  `max_pixels` は既定6000万px だが注入可（要件2.2「上限はすべて設定で緩和できる」）。
- 優先順位（AccessDenied ＞ SymlinkLoop ＞ CloudPlaceholder ＞ ImageTooLarge ＞ NetworkDrive ＞
  ReadOnly）を複数同時成立ケースで観測。`can_continue` は全縮退で true（クラッシュ・フリーズしない。
  要件12.1）を網羅ループで観測。
- テスト: 各ケース写像・画像ガード（内/外/境界/非画像/上限可変）・優先順位 5 組・継続性網羅・文言
  非空・決定論を 19 ケースで観測。

### must#2 ビュー別5状態（view_state）— 充足（自動テスト化済み）

`resolve_view_state(ViewStateInput)` を純粋関数として実装（ui-design 15章・要件10章）。
- 上書き優先（Error ＞ Partial ＞ Loading ＞ Empty ＞ Ideal）で 1 状態に解決。
- **Partial（機能縮退）と Error（致命的に表示不能）の区別**（ui-design 15章末尾）: AccessDenied＝Error
  （読込不能）。差分自動オフ・ベースライン未取得・巨大画像・クラウド・循環＝Partial（黙って切らず理由
  表示）。NetworkDrive/ReadOnly は本文表示自体は通常どおりのためメイン5状態を変えず（Ideal を妨げず）
  通知バー側で扱う。degrade_model の結果を畳むだけで縮退判定は重複させない（責務分離）。
- **Empty の3分岐**（ui-design 15章「3分岐で文言を変える」）: フォルダ未オープン（NoFolderOpened）／
  検索0件（SearchNoHits）／消化後（AllConsumed）を `folder_opened`/`is_search_mode`/`all_consumed` から
  決定し、`empty_reason_label` で 3 文言が相異なることをテストで観測。
- Loading は進捗（loaded_count/total_count）を透過（percent-done＋件数。UI は非ブロック）。
- テスト: Ideal・Empty 3分岐＋文言相異・Loading 進捗・Partial（4経路）・Error・NetworkDrive/ReadOnly が
  Ideal を妨げない・上書き優先 3 組・決定論を 17 ケースで観測。

### must#3 docs/acceptance.md（系統C 手動チェックリスト）— 充足

`docs/acceptance.md` を新規整備（design 13章・14章3・dev/spec.md 系統C）。
- 要求された項目をすべて収録: 起動500ms（A1）・TrySuspend 再表示300ms（A2）・WebView2 実描画（B群）・
  JS 有効無効切替の順序保証（B4）・Scintilla 実編集（C群）・ライブリロード実動（D群）・画像簡易ビュー
  実描画（I1/I2）・requirements 各章受け入れ基準（A〜K の各表に requirements 3.4/4.3/5.6/6.4/7.4/8.4/9.5/
  10.4/11.4/11.5/12 章を写経）。
- **各項目に対応する requirements/design/ui-design 節を併記**（criteria 要求）。系統A/B で済む判断と本書
  （系統C）が見る実機項目の境界も明記し、二重検証を避けた。

### must#4 ビルド（系統A/B verify）— 充足

- `ctest --preset x64-core-test`: 633 tests PASS（従来 597 ＋ 新規 36 件。82 suites。非回帰）。
- `cmake --build --preset x64-release`: コンパイル＋リンク成功（exit 0・/W4・警告エラー扱いで無警告）。
  新規 controller（degrade_model・view_state）は pika_core 経由で pika.exe／pika.com／pika_tests の全
  ターゲットにリンク済み。

## should criteria の状況

- **ラスター画像簡易ビュー（wxImage+自前描画・WebView2 不使用・ピクセル数ガード超過時は誘導）**: ガード
  判定（ImageTooLarge＝ヘッダ寸法でのデコード前ブロック・外部誘導）は degrade_model に実装・テスト済み。
  wxImage 実描画・等倍/フィット切替は GUI 実機のため acceptance.md I1/I2（系統C）へ集約。
- **診断ログ（error/warn/info・既定 warn 以上・ユーザー内容を書かない・5MB×3世代）**: util/logger は前回コア
  フェーズで実装済み（gtest 済み）。GUI からのログフォルダ起動・実ローテーションは acceptance.md I11（系統C）。
- **About 画面のサードパーティ OSS ライセンス表示**: 文言/表示は GUI 実機のため acceptance.md K1（系統C）。
- **中心体験がキーボードのみで完結（F6/Shift+F6 フォーカス循環）**: 実フォーカス循環は GUI 実機のため
  acceptance.md G4（系統C）。ショートカット割当の判断は前 sprint6 の controller/shortcut_table で gtest 済み。

これらの should は実描画・実 OS 操作が要りユニットテスト不能なため、spec.md の検証戦略（系統B コンパイル
成立＋系統C 手動 acceptance）に従い acceptance.md へ集約した。判断ロジック部分は本/既存 controller で gtest 済み。

## テスト化できなかった criteria とその理由

must 3 件（縮退判定・5状態・acceptance.md 整備）のうち、ロジック 2 件（must#1/#2）は wx 非依存の純粋
ロジックとして gtest 化済み。must#3（acceptance.md）は系統C のチェックリスト整備そのものが成果物であり
（自動テストの対象ではない＝系統C のフェーズ集約点）、内容の網羅性は本 report と spec.md 系統C 定義に対して
検証できる。テストの緩和・スキップ・無効化は行っていない（全 36 ケースが実挙動を観測。犠牲的期待値なし）。

## 自己実行した verify の結果

- `ctest --preset x64-core-test` → 100% tests passed（pika_tests_build＋pika_tests の 2/2）。
  内訳: pika_tests バイナリで 633 tests / 82 suites が PASS（新規 36 件＝DegradeModelTest 19・
  ViewStateTest 17 を含む）。
- `cmake --build --preset x64-release` → exit 0（コンパイル＋リンク成功・/W4 無警告。pika.exe/pika.com/
  pika_tests 全ターゲット生成）。

（合否判定の正本は run-dev が別途実行する結果。上記は自己確認。）

DONE: C:/dev/pika_editor/dev/turns/turn-8-1-generator.md
