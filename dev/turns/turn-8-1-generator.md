# turn-8-1 generator report — sprint 8（core/settings ＋ core/workspace）

## sprint goal（再注入）

core/settings と core/workspace を実装し、settings.toml の読み取り専用・不正値フォールバック、
ツリーモデル（自然順・除外リスト）・未読集合・rename引き継ぎを gtest で検証する。

中心体験の文脈（spec.md）: 外部変更を即時反映し変更ファイルをツリー上に未読表示する縦切りの土台。
core/settings は他コアの閾値・トグルの供給源、core/workspace は未読・ツリーのメモリモデル。

## 変更ファイル一覧

新規（実装）:
- `src/core/settings/settings.h` — `Settings` 値モデル・`LoadResult`・`load_settings`
- `src/core/settings/settings.cpp` — toml11 による読み取り専用ロード・型/範囲検証・既定フォールバック・直前値維持
- `src/core/workspace/workspace_model.h` — `natural_less`/`is_excluded`/`build_tree`/`UnreadSet`/`apply_renames`
- `src/core/workspace/workspace_model.cpp` — 上記の実装

新規（テスト）:
- `tests/core/settings/settings_test.cpp` — 9 ケース
- `tests/core/workspace/workspace_model_test.cpp` — 17 ケース

既存（ビルド登録）:
- `src/CMakeLists.txt` — settings/workspace のソース追加、`toml11::toml11` を PRIVATE リンク（公開ヘッダに toml 型を出さない）、`find_package(toml11 CONFIG REQUIRED)` 追加
- `tests/CMakeLists.txt` — 2 テストファイルを登録

依存追加なし: `toml11` は既に vcpkg.json に宣言済み（未リンクだったものを sprint8 で初使用）。

## must criteria の実装状況（すべて自動テスト化）

### core/settings
- 「settings.toml を読み取り専用で読み込み、不正値は既定値にフォールバックして警告フラグを返し
  起動不能にしない」→ 実装済み・テスト化済み。
  - 型違い: `SettingsTest.InvalidTypeFallsBackToDefaultWithWarning`（tabWidth=文字列・allowRemoteResources=整数）
  - 範囲外: `SettingsTest.OutOfRangeFallsBackWithWarning`（tabWidth=999・fontSize=1000）
  - 列挙外: `SettingsTest.UnknownEnumFallsBackWithWarning`
  - 負整数: `SettingsTest.NegativeIntegerFallsBack`
  - 読み取り専用（入力文字列を変更しない）: `SettingsTest.ReadOnlyDoesNotMutateInput`
  - いずれも例外を投げず `LoadResult.warnings` に該当キーを積み、起動不能にしない（既定で続行）
- 「保存途中の不完全な TOML でパース失敗時は直前の有効値を維持する」→ 実装済み・テスト化済み。
  - `SettingsTest.BrokenTomlKeepsPreviousValid`: 構文破損入力で `parse_ok=false`・`previous` をそのまま返し、
    既定への全戻し（ちらつき）も警告も出さない。

### core/workspace
- 「フォルダ先行・自然順ソート（file2 が file10 より前）」→ `natural_less`/`build_tree` 実装・テスト化済み。
  - `NaturalSortTest.*`（数値順 > 辞書順・大小無視・接頭辞・先頭ゼロ）、`BuildTreeTest.FoldersFirstThenNaturalOrder`
- 「既定除外リスト（.git/node_modules）が非表示かつ監視対象外」→ `is_excluded`/`build_tree` 実装・テスト化済み。
  - `ExcludeTest.DefaultsHideGitAndNodeModules`（途中セグメント除外・部分一致は除外しない）
  - `BuildTreeTest.ExcludedEntriesAreNotInTree`（除外配下を木に一切含めない＝監視対象外と同義のモデル）
- 「未読の rename 引き継ぎ（未読・ベースライン・退避を引き継ぎ、対応付け不能時は最終ディスク内容で
  再判定）」→ `apply_renames`/`CarryState`/`CarryOutcome` 実装・テスト化済み。
  - `RenameCarryTest.MovesUnreadBaselineAndStash`（未読・baselineHash・stash_ids を new へ移送）
  - `RenameCarryTest.OverwriteDestinationUsesSourceState`（移動先に既存があれば移動元で上書き）
  - `RenameCarryTest.OldAloneIsRemovedAndOrphanedForSafety`（旧名単独＝削除・旧キーで孤立保全）
  - `RenameCarryTest.NewAloneIsCreatedWithoutBaseline`（新名単独＝新規・ベースラインなし・未読）
  - `RenameCarryTest.RoundTripIsReevaluated`（A→B→A の逆向きペアを Reevaluated に倒し reevaluate へ）
  - `RenameCarryTest.SwapPreservesBothStates`（一時退避を介した正規スワップ A→tmp,B→A,tmp→B は双方引き継ぎ）

## should criteria の実装状況

- 「子孫に未読があるフォルダの伝播未読と、ファイル自身の未読を区別できるモデルである（gtest）」→ 充足。
  `UnreadSet::is_unread`（ファイル自身）と `has_unread_descendant`（子孫伝播）を別 API で提供し、
  `UnreadSetTest.SelfVsDescendantPropagation`・`PrefixIsNotSubstringMatch`（"docs" が "documents/x" を誤検出
  しない）で検証。
- 「除外リスト・閾値等の設定項目が settings から取得できる」→ 充足。`Settings` が除外リスト・巨大ファイル
  閾値（段階1/2）・行長・レンダリングガード閾値・スナップショット容量・機密パターン・unread.fullHashOnStartup・
  機能トグル等を保持し、`SettingsTest.ValidValuesAreApplied` で読み出しを検証。整合ガード（段階1<段階2）も
  `SettingsTest.Stage1MustBeBelowStage2` で検証。

## テスト化できなかった criteria

なし。must 5 件・should 2 件すべてを純ロジック（UI/実FS 非依存）として gtest 化した。
本 sprint の責務は「決定論部分」であり、settings.toml の実 FS 自己監視（SettingsWatcher、core/watcher を
直接使う design.md 2章の明示例外）の配線・ディスク列挙の実 FS 結合は後続 sprint（document/結合層）と
GUI/手動チェックリスト側に属するため本 sprint の must には含まれない。設計上はその上位が build_tree/
apply_renames/load_settings を呼び出す前提でモデル境界を切ってある。

## 自己実行した verify の結果

sprints.json sprint 8 の verify は `ctest --preset x64-core-test`。構成確認のため cmake も実行した。

- `cmake --preset x64-core-test` → exit 0（vcpkg manifest 解決・toml11 含む構成成立）
- `ctest --preset x64-core-test` → exit 0、`100% tests passed, 0 tests failed out of 2`
  （pika_tests_build フィクスチャで /W4・/WX ビルド成功 → pika_tests 実行）
- 新規スイートのみの実行: `26 tests from 6 test suites ran. [ PASSED ] 26 tests.`
  （SettingsTest 9 ＋ NaturalSortTest 4 ＋ ExcludeTest 1 ＋ BuildTreeTest 3 ＋ UnreadSetTest 3 ＋ RenameCarryTest 6）

合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

## 設計判断メモ（ドリフト追跡用）

- toml11 v4 では `toml::parse(std::string)` は文字列を「ファイル名」として扱うため、内容パースには
  `toml::parse_str` を使用した（誤って FS を触らせない）。
- 検証方針は「未指定キーは既定維持で無警告／指定キーが型違い・範囲外・列挙外のみ既定へフォールバック＋
  warnings へキー追加」。要件10.3「起動不能にしない」「既定値への全戻しでちらつかせない」を満たす。
- rename の往復（A→B→A）検出は、逆向きペア `{new→old}` の既出を見る決定論ロジックにした。グローバル
  出現数カウント方式は一時退避を介した正規スワップ（A→tmp,B→A,tmp→B）を誤って往復扱いするため不採用。
- 機密パターンの既定は core/snapshot/sensitive.cpp の既定（`.env*`/`*.env`/`*.key`/`*.pem`/`*secret*`）と
  一致させた（settings 側の sensitive_patterns 既定）。重複定義だが、責務は「設定の供給（settings）」と
  「判定（snapshot）」で分離しており、結合は上位層が担う設計のまま。

DONE: C:/dev/pika_editor/dev/turns/turn-8-1-generator.md
