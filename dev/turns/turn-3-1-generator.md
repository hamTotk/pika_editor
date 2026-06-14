# turn-3-1 generator report — sprint 3: core/watcher（技術リスク b）

## 概要

sprint 3 goal「技術リスク(b): core/watcher のイベント合成・自己保存抑制・rename正規化・
バッファオーバーフロー再同期・確定読みを UI 非依存ロジックとして実装し、テンポラリフォルダを使った
実FS gtest で検証する」を実装した。

設計原則に従い、watcher のコアロジックを wx・Win32 通知から切り離した純ロジックに分解し
（時刻・生イベント・ディスクハッシュを注入して決定論検証可能）、実 FS が必要な部分
（確定読みのプレスクリーン、オーバーフロー再同期の再列挙）のみ Win32/`std::filesystem` を使う
fs_probe・resync に閉じた。`ReadDirectoryChangesW` を所有する監視スレッド本体はプラットフォーム層の
責務（GUI 系）として本スプリント外（spec 補完判断 2・3 の core-test 系に整合）。

## 変更ファイル一覧

### 実装（src/core/watcher/ 新規）
- `src/core/watcher/fs_event.h` — `RawEvent`/`RawAction`・`FsEvent`/`FsEventKind`・`TimeMs` 型定義
- `src/core/watcher/event_synthesizer.h` / `.cpp` — デバウンス＋連続書き込みの 1 イベント合成
- `src/core/watcher/self_save_guard.h` / `.cpp` — 自己保存トークンのワンショット消費（ハッシュ一致主条件）
- `src/core/watcher/rename_tracker.h` / `.cpp` — old/new ペア突き合わせと安全側フォールバック
- `src/core/watcher/settle_reader.h` / `.cpp` — 確定読み（静穏期間＋mtime/サイズ安定）
- `src/core/watcher/fs_probe.h` / `.cpp` — 軽量メタ取得（mtime/size）＋LF正規化内容ハッシュ（実FS）
- `src/core/watcher/resync.h` / `.cpp` — オーバーフロー再同期／ポーリングの全再列挙突き合わせ（実FS）
- `src/core/watcher/watcher_core.h` / `.cpp` — 上記を束ねる UI 非依存パイプライン（合成→rename→自己保存抑制）

### テスト（tests/core/watcher/ 新規）
- `event_synthesizer_test.cpp`・`self_save_guard_test.cpp`・`rename_tracker_test.cpp`・
  `settle_reader_test.cpp`（純ロジック）
- `fs_probe_test.cpp`・`resync_test.cpp`（テンポラリフォルダの実FS）
- `watcher_core_test.cpp`（パイプライン結合・モックディスク注入）

### ビルド設定
- `src/CMakeLists.txt` — pika_core に watcher 8 モジュールを追加
- `tests/CMakeLists.txt` — pika_tests に watcher 7 テストファイルを追加

いずれも project_dir 内のみ。spec.md / sprints.json / ref-dev / eval JSON は読み取りのみ。

## must criteria 実装状況（すべて自動テスト化）

| must | 実装/テスト | 状況 |
|------|------------|------|
| イベント合成（連続書き込み→デバウンス100ms→1 FsEvent） | `EventSynthesizer` / `event_synthesizer_test.cpp`（`CoalescesBurstWritesIntoOneEvent` ほか） | OK |
| 自己保存抑制（ハッシュ一致主条件・ワンショット・窓超過でも一致なら抑制・内容相違は外部変更） | `SelfSaveGuard` / `self_save_guard_test.cpp`（`ConsumesSelfWhenHashMatches`・`SuppressesEvenBeyondTimeWindowWhenHashMatches`・`ExternalChangeNotSuppressed`・`MultipleSavesConsumeOneAtATime`）＋結合 `watcher_core_test.cpp` | OK |
| rename 正規化（窓内ペア追従・old単独=削除・new単独=新規・窓外不成立） | `RenameTracker` / `rename_tracker_test.cpp`（`PairsOldNewWithinWindow`・`PairsWhenNewArrivesFirst`・`LoneOldBecomesRemoved`・`LoneNewBecomesCreated`・`OutOfWindowOldNewDoNotPair`） | OK |
| バッファオーバーフロー再同期（全再列挙→mtime/サイズ→ハッシュ比較で取りこぼさない） | `resync()` / `resync_test.cpp`（`DetectsAddedModifiedRemovedAfterOverflow`・`NewlineOnlyChangeIsNotModified`・`NoMissAtScale`） | OK（実FS） |
| 確定読み（静穏期間＋mtime/サイズ安定まで中途内容で確定しない） | `SettleReader` / `settle_reader_test.cpp`（`DoesNotConfirmWhileSizeStillGrowing`・`ConfirmsAfterQuietAndStable`・`MtimeChangeResetsStability`） | OK |

上書き rename（既存への上書き）は「呼び出し側が存在判定して内容変更に倒す」契約とし、watcher 側は
new 単独＝Created を返す設計（design.md 5.2 の安全側正規化に一致。WorkspaceController が反映時に
存在判定する責務）。本スプリントでは WorkspaceController が無いため、その判定は sprint 8 へ送る。

## should criteria 実装状況

| should | 状況 |
|--------|------|
| 監視不能環境向け定期ポーリング（mtime+サイズプレスクリーン再利用） | `resync()` を定期実行で再利用する設計とし、同関数がプレスクリーン共有を実装（`resync.h` 冒頭コメント・F5/ポーリング兼用）。専用クラスは設けず関数共有で YAGNI 抑制 |
| N件（100規模）同時変更で取りこぼしなし | `resync_test.cpp::NoMissAtScale`（100件一斉変更→100件 Modified）で確認済み |

## テスト化できなかった criteria とその理由

- なし（must は全件 gtest 化）。
- 実機要素（`ReadDirectoryChangesW` の `ERROR_NOTIFY_ENUM_DIR` 実発火・共有違反リトライの
  実タイミング・監視スレッドの所有）は GUI/プラットフォーム層に属し、spec「検証手段」で
  should/`docs/acceptance.md` 側へ寄せる方針。本スプリントは溢れ「後」の再構成ロジック（resync）を
  決定論検証する形で must を満たした。

## 自己実行した verify の結果（自己確認・正本は run-dev）

- `cmake --preset x64-core-test` → 構成成立（exit 0。vcpkg manifest 解決＋CMake 構成 done）
- `ctest --preset x64-core-test` → 100% passed（2/2 テスト: `pika_tests_build` 成功 →
  `pika_tests` 成功）。watcher 関連は 41 tests / 7 suites がすべて PASS、リポジトリ全体で 96 tests PASS
- `clang-format --dry-run --Werror`（watcher の src・tests 全ファイル）→ 違反なし
- pika_core を /W4 /WX 下で再ビルド → 警告・エラーなし

### 補足（実装の自己レビュー観点）
- 純ロジック（synth/rename/self-save/settle）は時刻・ハッシュを注入し決定論。実FS（fs_probe/resync）は
  テンポラリフォルダで隔離（並行実行衝突を `this` アドレスで回避、既存 util テストの規約に揃えた）。
- LF正規化ハッシュ照合により「改行のみの差」を内容変更と誤らない（`NewlineOnlyChangeIsNotModified`）。
- 自己保存抑制は削除/rename を対象外にし、外部削除・移動を握り潰さない（`LoneRenameOldBecomesRemovedNotSuppressed`）。

## テスト修正に関する注記（緩和ではなく自テストの誤り訂正）

初回実行で `RenameTrackerTest.FlushAllDrainsPending` が失敗した。原因は**テスト側の誤り**で、
同時刻（窓内）の `on_old`→`on_new` は仕様どおり即ペア成立（Renamed）して保留が残らないのに、
テストが「2 件の単独保留が残る」ことを期待していた。実装の挙動が正しいため、テストを
「窓外（時刻 0 と 1000）の old/new は互いにペアにならず単独保留として残る」入力に修正した
（テストの緩和・スキップ・無効化ではなく、観測対象を仕様に合わせた訂正）。修正後 100% パス。

DONE: C:/dev/pika_editor/dev/turns/turn-3-1-generator.md
