# turn-4-1 generator report — sprint 4（中心体験②『外部変更を反映』のアプリケーション層と Win32 監視配線）

## sprint goal（再注入）

WorkspaceController（watcher の FsEvent を受けて未読/ツリー更新、自己保存トークン照合、rename 追従）の
wx 非依存ロジックをテストし、ReadDirectoryChangesW の監視スレッド実体（FILE_NOTIFY_INFORMATION→RawEvent→
WatcherCore::on_raw）とポーリングフォールバック/F5 を配線する（depends_on: [3]）。
must verify は系統A `ctest --preset x64-core-test` ＋ 系統B `cmake --build --preset x64-release`（/W4・警告エラー）。

spec.md「検証戦略」に従い、反映判断のうち wx/Win32/実 FS 非依存に切り出せる部分を controller へ厚く寄せて
決定論ゲート（系統A）に乗せ、ReadDirectoryChangesW の実配線は系統B（コンパイル＋リンク成立）で検証した。

## 変更ファイル一覧

新規（controller・wx/Win32 非依存ロジック＝pika_core に含め gtest 対象。系統A）:
- `C:/dev/pika_editor/src/controller/workspace_controller.h` / `.cpp` — `WorkspaceController`：FsEvent
  （Created/Modified/Removed/Renamed）を受けて UnreadSet と引き継ぎ状態（CarryState）を更新。rename 追従は
  core/workspace `apply_renames` で relPath 付け替え（未読・ベースライン・退避を引き継ぐ）。削除は消失タブ
  安全遷移（PathRemoved）＋状態の孤立保全。新規（◆・ベースラインなし）と差分あり（±・ベースラインあり）を
  `new_files()` で弁別。`build_view_model()` で sprint1 の `build_tree_view_model` を現状態で呼ぶ。
  resync 突き合わせ基準の `baseline()` を保持。
- `C:/dev/pika_editor/src/controller/watch_event_map.h` / `.cpp` — `make_raw_event`/`map_watch_action`/
  `normalize_watch_rel_path`：FILE_NOTIFY_INFORMATION の Action コード（FILE_ACTION_*）→ RawAction 写像と
  '\\'→'/' 区切り正規化・先頭/末尾区切り除去。未知コード/空パスは破棄（nullopt）。

新規（テスト。系統A）:
- `C:/dev/pika_editor/tests/controller/workspace_controller_test.cpp` — 13 ケース（Created=◆/Modified+baseline=±/
  baseline 無し Modified の◆フォールバック・Removed の PathRemoved＋未読解除＋状態保全・rename での未読/ベースライン/
  退避引き継ぎ・mark_confirmed・new_files 弁別・ViewModel での Diff vs DiffPropagated 区別・New マーク・baseline 保持・
  自己保存抑制で未読化しない vs ハッシュ不一致で未読化）。
- `C:/dev/pika_editor/tests/controller/watch_event_map_test.cpp` — 9 ケース（既知/未知 Action 写像・区切り正規化・
  先頭末尾除去・空/ルート→空・UTF-8 多バイト保持・make_raw_event の合成と破棄条件）。

新規（プラットフォーム層＝実 OS/FS I/O。系統B/C）:
- `C:/dev/pika_editor/src/app/watch_thread.h` / `.cpp` — `WatchThread`：ReadDirectoryChangesW（OVERLAPPED・64KB
  バッファ・サブツリー監視）を回し、FILE_NOTIFY_INFORMATION を 1 件ずつ UTF-16→UTF-8 化（Win32 のみ本ファイルに残す）→
  `controller::make_raw_event` で RawEvent へ正規化→ on_raw コールバックで UI スレッドへ。監視開始不能環境は定期
  ポーリング（既定5秒）へ自動フォールバックし on_resync(Poll)。ERROR_NOTIFY_ENUM_DIR（バッファ溢れ）で on_resync(Overflow)。
  `request_resync()`＝F5（on_resync(Manual)）。stop で CancelIoEx＋スレッド join。

変更（UI 層＝wx 依存。系統B/C）:
- `C:/dev/pika_editor/src/ui/main_frame.h` / `.cpp` — MainFrame に WorkspaceController・WatcherCore（HashProbe＝
  content_hash_lf）・WatchThread を所有させ結線。`start_watching`（フォルダ表示後に監視開始）／`stop_watching`
  （フォルダ切替・終了）／`on_raw_event`（WatcherCore::on_raw→poll→反映・UI スレッド）／`on_resync_needed`
  （drain→resync(baseline)→反映・進捗ステータス）／`apply_fs_events`（PathRemoved→TabManager::mark_path_missing・
  ツリー/未読件数更新）／`on_refresh`（F5＝request_resync）。デバウンス窓（既定100ms）経過後の確定取りこぼしを
  単発 wxTimer（150ms）で拾う。refresh_tree は WorkspaceController::build_view_model で未読マークを反映。
- `C:/dev/pika_editor/src/ui/ui_messages.h` / `.cpp` — MenuRefresh・StatusWatching/Polling/Syncing を追加
  （生文字列を散らさず ID→日本語の単一定義経由。design 10章 K9・F3 の監視/再同期進捗表示）。
- `C:/dev/pika_editor/src/CMakeLists.txt` — pika_core に workspace_controller・watch_event_map（4 ソース）を追加。
- `C:/dev/pika_editor/src/app/CMakeLists.txt` — pika_ui に watch_thread を追加。
- `C:/dev/pika_editor/tests/CMakeLists.txt` — pika_tests に新規 2 テストソースを追加。

注: `src/core/`・`docs/`・`dev/spec.md`・`dev/sprints.json`・`dev/turns/*-eval.json`・`ref-dev` は一切改変していない
（`git diff --stat -- src/core docs dev/spec.md dev/sprints.json` が空・eval/ref は未 touch）。`dev/state.json` は
run-dev のループ状態であり本実装の書き込み対象ではない。

## must criteria の実装状況

1. **WorkspaceController: poll() の FsEvent を受け、未読集合（UnreadSet）とツリー ViewModel を更新。自己保存
   トークン照合（ハッシュ一致・ワンショット）の消し込み** — 達成（系統A）。`WorkspaceController::apply_events` が
   FsEvent を未読集合・引き継ぎ状態へ写し、`build_view_model` が現状態で sprint1 ViewModel を組む。自己保存照合は
   WatcherCore（既存・gtest 済み）の `register_self_save`＋`poll` の消し込みを WorkspaceController と組み合わせて観測：
   `SelfSaveSuppressedDoesNotMarkUnread`（ディスクハッシュ＝保存後ハッシュ一致→poll が FsEvent を出さず未読化しない）／
   `ExternalChangeMarksUnreadWhenHashDiffers`（不一致→Modified が確定し未読化）。

2. **rename 追従: apply_renames で index relPath 付け替え（未読・ベースライン・退避の引き継ぎ）。rename 後に未読が
   引き継がれる** — 達成（系統A）。`apply_events` の Renamed 分岐が core/workspace `apply_renames` を呼び states_ を
   付け替え、未読も旧→新へ移す。`RenameCarriesUnreadToNewPath`（未読が renamed.md へ移る・旧キー消滅・baseline_hash
   引き継ぎ）／`RenameCarriesStashIds`（CarryState が新キーへ）で観測。

3. **ReadDirectoryChangesW 監視スレッド実体: FILE_NOTIFY_INFORMATION を UTF-8/'/'区切りに正規化し TimeMs を付して
   RawEvent として WatcherCore::on_raw へ供給** — 達成（系統B/C＋系統A の写像部）。`WatchThread::run` が
   FILE_NOTIFY_INFORMATION を走査し UTF-16→UTF-8（Win32）→`make_raw_event`（'/'区切り正規化・TimeMs=GetTickCount64）→
   on_raw。写像/正規化の wx/Win32 非依存部は `watch_event_map` へ切り出し 9 ケースで gtest 観測（UTF-8 多バイト保持・
   未知コード破棄・先頭末尾区切り除去）。実 ReadDirectoryChangesW の起動・実通知受信は実 OS/FS が要り系統B/C。

4. **監視不能環境のポーリングフォールバック（既定5秒）と F5 が同じ再同期処理（resync）をオンデマンド実行。バッファ
   オーバーフロー（ERROR_NOTIFY_ENUM_DIR）で全再列挙→再同期** — 達成（系統B/C）。`WatchThread` は open_dir 失敗時
   ポーリング（既定 kDefaultPollIntervalMs=5000ms）へ転落し on_resync(Poll) を送る。ERROR_NOTIFY_ENUM_DIR/件数0で
   on_resync(Overflow)。`request_resync`＝F5。MainFrame の `on_resync_needed` がいずれの理由でも drain_for_resync→
   `core/watcher::resync(root, baseline)`→apply_events と同一処理を実行する。

5. **ctest --preset x64-core-test が PASS し、cmake --build --preset x64-release がコンパイル＋リンク成功（/W4 exit 0）** —
   達成。下記「自己実行した verify」参照。

## should criteria の実装状況

- **クリーンタブへの外部変更を単一 Undo で反映し dirty にしない** — 部分。Scintilla 内容差し替えの単一 Undo 化は
  実エディタ操作（系統C・acceptance.md）。本 sprint は WorkspaceController が未読/削除/rename を確定し、PathRemoved を
  TabManager::mark_path_missing へ橋渡しする土台までを入れた（クリーンタブ自動リロードの実反映は差し替え API 配線時）。
- **監視/ポーリング/F5 実行中の進捗をステータス/通知バーに表示（design 10章 F3）** — 達成（ステータス側）。
  StatusWatching/Polling/Syncing を update_status と on_resync_needed で表示。通知バー集約は sprint7 計画。
- **監視スレッドは重い処理をせずイベント合成してキューに積むだけ（design 4章）** — 達成。WatchThread は正規化と
  コールバック転送のみ。合成・自己保存抑制・確定読み・resync は UI スレッド側（WatcherCore は非スレッドセーフ）。

## テスト化できなかった criteria とその理由

- **must 3（ReadDirectoryChangesW の実起動・実通知受信）・must 4（実 UNC/クラウドでのポーリング転落・実バッファ溢れ）** —
  実 OS/実 FS が要りユニットテスト不能（design 13章「UI/実 OS は初期版で自動テストしない」）。spec の検証戦略どおり
  系統B（`cmake --build --preset x64-release` exit 0＝型整合・シンボル解決）で配線を検証し、実挙動は系統C
  （docs/acceptance.md・sprint8 で整備）へ委ねた。決定論側は、Action 写像/区切り正規化（watch_event_map・9 ケース）と
  FsEvent→未読/rename/削除の反映（workspace_controller・13 ケース）を wx/Win32/実 FS 非依存関数に切り出し観測している。
- **should（クリーン自動リロードの単一 Undo 復帰・Ctrl+Z）** — Scintilla 実編集が要るため系統C（acceptance.md）。

## 自己実行した verify の結果

- `cmake --preset x64-core-test` / `cmake --preset x64-release` — いずれも構成成功（Configuring/Generating done）。
- **`ctest --preset x64-core-test` — exit 0 / 100% passed（2/2 ctest）。gtest 493 件 PASS / 0 失敗（72 スイート）。**
  新規 2 スイート＝WorkspaceControllerTest 13・WatchEventMapTest 9＝計 22 件、差し引き 471 件が sprint3 到達点の非回帰 PASS。
- **`cmake --build --preset x64-release` — exit 0（/W4 /WX 警告エラーなし）。pika.exe・pika.com・pika_ui.lib・
  pika_core.lib・pika_tests.exe を生成。** MainFrame が WorkspaceController/WatcherCore/WatchThread（sprint4）と
  TabManager（sprint2）・build_tree_view_model（sprint1）を呼ぶ結線がコンパイル＋リンク成立。
- 全 12 触 C++ ファイルの `clang-format --dry-run --Werror` clean。

合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

DONE: C:/dev/pika_editor/dev/turns/turn-4-1-generator.md
