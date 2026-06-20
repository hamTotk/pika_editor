# turn-1-1 generator report — sprint 1（controller 層新設 + TreeViewModel）

## sprint goal（再注入）

controller 層を pika_core（wx 非依存）に新設し pika_tests から gtest 検証できるビルド構成を確立する。
最初の wx 非依存 ViewModel として「ツリー→ViewModel 変換」（core/workspace の build_tree/UnreadSet
結果を状態マーク ±/◆/取消線・伝播±淡・種別アイコン分類へ写像）を実装しテストする。

## 変更ファイル一覧

新規:
- `C:/dev/pika_editor/src/controller/tree_view_model.h` — TreeViewModel の型と純粋関数 API
- `C:/dev/pika_editor/src/controller/tree_view_model.cpp` — 状態マーク解決・アイコン分類・木変換の実装
- `C:/dev/pika_editor/src/controller/tree_view_messages.h` — 列挙値→記号/日本語ラベルの単一定義 API
- `C:/dev/pika_editor/src/controller/tree_view_messages.cpp` — その実装
- `C:/dev/pika_editor/tests/controller/tree_view_model_test.cpp` — 新規 gtest（18 ケース）

変更:
- `C:/dev/pika_editor/src/CMakeLists.txt` — pika_core STATIC に controller の 4 ソースを追加（GUI ブロックの外＝PIKA_BUILD_GUI 非依存）
- `C:/dev/pika_editor/tests/CMakeLists.txt` — pika_tests に controller/tree_view_model_test.cpp を追加

## must criteria の実装状況

1. **src/controller/ 新設・pika_core に含む・x64-core-test でビルド可** — 達成。
   `src/CMakeLists.txt` の `add_library(pika_core STATIC ...)`（GUI ブロック `if(PIKA_BUILD_GUI)` の
   外側）に controller ソースを追加。`cmake --preset x64-core-test`（PIKA_BUILD_GUI=OFF）→ pika_core
   ビルド成功を確認。x64-release（PIKA_BUILD_GUI=ON・/W4 /WX）でも pika_core ターゲット単体ビルド成功
   （警告エラーなし）。
2. **tests/controller/ の新規 gtest が pika_core にリンクし PASS・既存非回帰** — 達成。
   `ctest --preset x64-core-test` exit 0。`pika_tests.exe` 実行で **合計 409 件 PASS / 0 失敗**。
   うち新規 4 スイート 18 件（ClassifyIconTest/ResolveFileMarkTest/BuildTreeViewModelTest/
   TreeViewMessagesTest）。差し引き 391 件が既存テストで非回帰 PASS（前回到達点と一致）。
3. **TreeViewModel: build_tree()+UnreadSet → 状態マーク決定論付与（純粋関数）** — 達成。
   `build_tree_view_model(root, unread, new_files)` を実装。`PureFunctionSameInputSameOutput` で
   同一入力→構造的同一出力を再帰比較で観測。
4. **重畳優先「削除済み ＞ 未保存 ＞ 差分あり」** — 達成。`resolve_file_mark()` に一元化。
   `PriorityDeletedBeatsAll`（deleted+unsaved+unread→Deleted）・`PriorityUnsavedBeatsDiff`
   （unsaved+unread→Unsaved）で優先順位を観測。
5. **種別アイコン分類（folder/code/data/config/script/image/text/unknown）・未知は generic フォールバック** — 達成。
   `classify_icon()` を ui-design 6章の表で実装。`RepresentativeExtensionsMapToCategory`（全カテゴリの
   代表拡張子）・`UnknownExtensionFallsBackToUnknown`（表外/拡張子なし/先頭ドットのみ/末尾ドット→Unknown）で観測。
   フォルダは木変換側で IconCategory::Folder を付与（`IconClassificationPropagatedToNodes`）。
6. **フォルダ伝播±（淡）と ファイル自身±（実心）を別値で区別** — 達成。
   ファイル自身は `StateMark::Diff`、フォルダ伝播は `StateMark::DiffPropagated` の別値。
   `FileSelfDiffVsFolderPropagatedAreDistinct`（src フォルダ=DiffPropagated・src/a.md=Diff・EXPECT_NE で別値）と
   `NestedPropagationReachesAncestors`（a/b/c/deep.md の祖先全フォルダへ伝播・葉ファイルは実心 ±）で観測。
   伝播判定は core の `UnreadSet::has_unread_descendant()` をそのまま利用（再実装しない）。

## should criteria の実装状況

- **cmake --preset x64-release で GUI 構成が成立** — 達成。configure done を確認。pika_core ターゲット
  （controller 込み）も x64-release でビルド成功。
- **ViewModel 文字列を単一メッセージ定義（ID→日本語）経由にできる構造・生文字列を散らさない（design 10章 K9）** — 達成。
  ViewModel は表示属性を列挙値（StateMark/IconCategory）で返し文字列を持たない。記号/日本語ラベルは
  `tree_view_messages.{h,cpp}` の 1 箇所に集約（state_mark_symbol/state_mark_label/icon_category_label）。
  `TreeViewMessagesTest` で写像を観測。
- **自然順ソート・既定除外は core の natural_less/is_excluded をそのまま使い ViewModel 側で再実装しない（足さない）** — 達成。
  木の整列は build_tree（core）の結果順を保持するだけ。`PreservesFolderFirstNaturalOrder`（file2 が
  file10 より前・フォルダ先行）で観測。ViewModel 側にソート/除外ロジックは持たない。

## テスト化できなかった criteria とその理由

なし。本 sprint の must/should は全て wx 非依存の純粋ロジックであり、gtest で観測可能。
（NodeStateInput の deleted/unsaved は本 sprint の build_tree+UnreadSet 経路では常に false だが、
重畳優先の解決ロジックは resolve_file_mark に一元化済みで、sprint2 の TabManager 合流時にそのまま
使える形にしてある。優先順位そのものはテストで観測済み。）

## 自己実行した verify の結果

- `cmake --preset x64-core-test` — 構成成功（configure done）
- `ctest --preset x64-core-test` — **exit 0 / 100% passed（2/2 ctest テスト、gtest 409 件 PASS / 0 失敗）**
- 参考（should）: `cmake --preset x64-release` 構成成功 + `cmake --build build/x64-release --target pika_core`
  （/W4 /WX）成功（controller 込み・警告エラーなし）

合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

DONE: C:/dev/pika_editor/dev/turns/turn-1-1-generator.md
