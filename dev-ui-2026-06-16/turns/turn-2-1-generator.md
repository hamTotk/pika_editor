# turn-2-1 generator report — sprint 2（中心体験①『開く』のアプリケーション層を決定論で固める）

## sprint goal（再注入）

AppController/データルート解決/CLI受領/単一インスタンス判定の wx 非依存ロジックと、TabManager
状態機械（タブ追加・削除・アクティブ遷移・重畳状態の表示優先・消失タブの安全遷移）を controller に
実装しテストする（depends_on: [1]）。系統A（決定論ゲート）の must verify は `ctest --preset
x64-core-test`。実パイプ I/O・MainFrame 配線は sprint3 に委ねる。

## 変更ファイル一覧

新規（実装）:
- `C:/dev/pika_editor/src/controller/data_root.h` — DataRootProbe→DataRoot 解決 API（portable.txt 分岐）
- `C:/dev/pika_editor/src/controller/data_root.cpp` — その純粋関数実装
- `C:/dev/pika_editor/src/controller/app_controller.h` — CLI 受領（OpenPlan）・単一インスタンス判定（InstanceDecision）・起動手順列 API
- `C:/dev/pika_editor/src/controller/app_controller.cpp` — core/ipc（parse_argv/validate/parse_goto/normalize_to_absolute/make_pipe_name/serialize_request）を組み合わせた純粋実装
- `C:/dev/pika_editor/src/controller/tab_manager.h` — TabManager 状態機械・display_mark・FolderSwitch 状態機械 API
- `C:/dev/pika_editor/src/controller/tab_manager.cpp` — その実装

新規（テスト）:
- `C:/dev/pika_editor/tests/controller/data_root_test.cpp` — 5 ケース（両分岐＋正規化＋未確定）
- `C:/dev/pika_editor/tests/controller/app_controller_test.cpp` — 13 ケース（CLI 正規化・単一インスタンス・起動順序）
- `C:/dev/pika_editor/tests/controller/tab_manager_test.cpp` — 22 ケース（open/close/activate・消失安全遷移・重畳優先・フォルダ切替）

変更:
- `C:/dev/pika_editor/src/CMakeLists.txt` — pika_core STATIC に sprint2 の 3 実装（6 ソース）を追加（GUI ブロック `if(PIKA_BUILD_GUI)` の外＝PIKA_BUILD_GUI 非依存。決定論ゲート側）。controller のロードマップ注記を sprint2 まで更新
- `C:/dev/pika_editor/tests/CMakeLists.txt` — pika_tests に新規 3 テストソースを追加

注: `dev/state.json` の差分は run-dev のループ状態であり本実装の書き込み対象ではない。`src/core/` の
コアロジック・spec.md・sprints.json・ref-dev・eval JSON は一切改変していない（git status で確認）。

## must criteria の実装状況

1. **DataRoot 解決（portable.txt 分岐の両方をテストで観測。design 5.1・7章 K1）** — 達成。
   `resolve_data_root(DataRootProbe)` を純粋関数化。`portable_marker_present=true`→`<exe_dir>\pika-data`
   （Portable）、false→`<local_app_data>\pika`（LocalAppData）。FS 実在判定（portable.txt 有無・環境変数）は
   呼び出し側が bool/文字列で注入しコアを FS 非依存に保つ。`PortableMarkerSelectsExeAdjacentPikaData`
   /`NoMarkerSelectsLocalAppDataPika` で両分岐、`ForwardSlashesAndTrailingSeparatorsNormalized` で区切り
   統一・末尾畳み、`MissingLocalAppDataIsUnresolved`/`PortableWithoutExeDirIsUnresolved` で「退避先未確定なら
   resolved=false（データを失わない原則：退避先がないまま書き込みを始めない）」を観測。

2. **CLI 受領→OpenTarget 正規化（相対パスの絶対化を含めテストで観測。要件3.2）** — 達成。
   `plan_open(CliContext, PathProbe)` が core/ipc の `parse_argv`→`validate`→`normalize_to_absolute` を
   組み合わせ、各引数を cwd 基準で絶対パス化して `OpenTarget`（path・line・column）へ落とす。`-g` の
   位置指定は validate が `parse_goto` 経由で剥がし、ドライブレター直後のコロンは分割しない。
   観測: `RelativeFileIsAbsolutizedAgainstCwd`（"notes.md"+cwd→"C:\\work\\proj\\notes.md"）、
   `GotoCarriesCursorPosition`（:42:7→line=42/column=7）、`DriveLetterColonNotSplitAsPosition`、
   `ExistingFolderBecomesWorkspace`、`NonexistentFileIsAcceptedAsNewTab`（新規タブ受理）、
   `NoArgsRequestsRestorePrevious`（引数なし＝前回状態復元）、`MissingFolderIsRejected`/
   `UnknownOptionIsRejectedWithoutGui`（GUI 非起動で非0終了）、`HelpIsAcceptedWithoutTargets`。

3. **単一インスタンス判定ロジック（make_pipe_name/serialize_request でサーバー/クライアント役割決定＋転送 JSON 組み立て。実パイプ I/O は sprint3）** — 達成。
   `decide_instance(InstanceContext, OpenPlan)` を wx/Win32 非依存関数化。`pipe_acquired=true`→Server
   （pipe_name のみ・転送 JSON なし）、false→Client（pipe_name＋`serialize_request` で OpenPlan の絶対パス化
   済み対象を 1 行 JSON 化・敗者終了コード0）。観測: `AcquiredBecomesServerWithPipeName`、
   `LoserBecomesClientAndBuildsTransferJson`（転送 JSON が改行を含まず、受信側 `parse_request` で往復して
   絶対パス・line・column・goto が保たれる＝信頼境界スキーマ整合を観測）、`ClientTransfersFolderTarget`
   （フォルダも絶対パスで転送）。CreateNamedPipe の原子的ロックと敗者フォールバック転送は design 5.1 手順2
   準拠。実 I/O 配線は sprint3。

4. **TabManager 状態機械（open/close/activate＋重畳表示優先 削除済み＞未保存＞差分あり、消失タブ安全遷移でクラッシュしない）** — 達成。
   `TabManager`（open=重複オープン抑止＋アクティブ移動／close=アクティブ閉鎖時は右隣優先→無ければ左隣の安全
   遷移・範囲外は no-op／activate=範囲外無視）と `mark_path_missing`（消失タブを閉じず削除済み表示で残し
   未確認内容を保持・アクティブ消失でも遷移破綻しない。design 5.1 手順4・要件7.2）。表示マークは
   `display_mark(TabState)` が tree_view_model の `resolve_file_mark` へ写して重畳優先を一元化（記号体系を
   ツリーとタブで分散させない）。観測: `Close*`（4 ケースで右隣/左隣/非アクティブ繰り上がり/全閉鎖→kNoActive）、
   `Missing*`（削除済み遷移・アクティブ消失非クラッシュ・未知パス no-op）、`DisplayMarkTest`（5 ケースで
   Deleted>Unsaved>Diff、新規◆弁別、クリーン None）、`SettersDriveOverlayPriority`（set_unread→set_unsaved→
   mark_path_missing の重畳順を観測）。範囲外 index/未知 path は全経路で安全（クラッシュしない）。

5. **`ctest --preset x64-core-test` が PASS（新規含め全件）** — 達成。下記「自己実行した verify の結果」参照。
   gtest 合計 449 件 PASS / 0 失敗（sprint1 の 409 件は非回帰 PASS＋新規 40 件）。

## should criteria の実装状況

- **AppController の起動手順（データルート解決→CLI解析→単一インスタンス判定→表示前検証）の順序が design 5.1 と一致** — 達成。
  `startup_sequence()` を順序付き `StartupStep` 列（ResolveDataRoot→ParseCli→DecideInstance→ShowWindow→
  AsyncLoad）で返し、`StartupSequenceTest.MatchesDesignOrder` で正典順序を観測。ウィンドウ表示（手順3）の後に
  ツリー列挙・監視（手順4＝AsyncLoad）が来る最短経路順を固定。
- **フォルダ切替（design 5.6）の状態遷移（未保存確認→後始末→新フォルダ列挙開始）を状態機械として表現しテスト** — 達成。
  `FolderSwitch`（begin で has_unsaved→ConfirmUnsaved/TeardownCurrent 分岐、resolve_unsaved で
  SaveAll/Discard→TeardownCurrent・Cancel→Cancelled、teardown_done で→EnumerateNew、確認段階外の選択は
  安全側無視）。`FolderSwitchTest` 5 ケースで全遷移とキャンセル中止（現ワークスペース維持）を観測。

参考（sprint2 の must verify ではないが構成健全性の確認）:
- `cmake --preset x64-release`（GUI 構成）成功。`cmake --build build/x64-release --target pika_core`
  （/W4 /WX）で sprint2 の 3 実装が警告エラーなしでコンパイル成功（sprint3 の GUI 配線に向けた前倒し確認）。

## テスト化できなかった criteria とその理由

なし。sprint2 の must/should はすべて wx・Win32・FS 非依存の純粋ロジック（データルート分岐・CLI 正規化・
役割決定＋JSON 組み立て・タブ状態機械・フォルダ切替状態機械）であり gtest で観測可能。実 I/O（CreateNamedPipe
の実獲得・MainFrame 配線・実 FS 列挙）は sprint3 以降の系統B/Cへ明示的に委ねた（criteria 文言「実パイプ I/O
は sprint3 で配線」と整合）。FS/Win32 依存箇所は PathProbe・InstanceContext.pipe_acquired・DataRootProbe の
注入境界に切り出し、判断ロジック自体を決定論側へ寄せている。

## 自己実行した verify の結果

- `cmake --preset x64-core-test` — 構成成功（Configuring/Generating done）
- `ctest --preset x64-core-test` — **exit 0 / 100% passed（2/2 ctest テスト：pika_tests_build＋pika_tests）。
  gtest 449 件 PASS / 0 失敗（66 スイート）**。新規 7 スイート（DataRootTest 5・PlanOpenTest 9・
  DecideInstanceTest 3・StartupSequenceTest 1・TabManagerTest 14・DisplayMarkTest 5・FolderSwitchTest 5）＝
  計 40 件、差し引き 409 件が sprint1 到達点の非回帰 PASS。
- 参考（should/構成健全性）: `cmake --preset x64-release` 構成成功＋`pika_core`（controller 込み・/W4 /WX）
  release ビルド成功（警告エラーなし）。

合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

DONE: C:/dev/pika_editor/dev/turns/turn-2-1-generator.md
