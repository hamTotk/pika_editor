# turn 10-1 generator report — sprint 10（退避フロー結合 ＋ 状態永続化）

## sprint goal（再注入）

退避フロー結合（DocumentController×snapshot 相当のコアロジック）と状態永続化（state.json/index.json の
version・アトミック書き込み・マイグレーション安全側）を実装し gtest で検証する。中心体験
（spec.md「中心体験」4「前回確認時点からの累積差分を確認し、確認済みにする」5「軽く修正して保存」）の
衝突・巻き戻し・復元を決定論的に固める。設計原則1「データを失わない」最優先（退避＝最後の砦）。

## スコープ判断（目標の文脈維持）

design.md 5.3/5.4/7章・要件7.3/8.3/9章/10章に従い、UI 非依存のコアロジックとして 2 モジュールを実装した。
実 FS への読み書き（タブ内容・ディスク反映）は呼び出し側 DocumentController が担い、本層は「ディスク内容
（std::string）」を入力にした純オーケストレーション（DiffEngine が内容文字列を受ける設計と同型）にして
wx/WebView2/Win32 非依存にし、gtest で決定論検証可能にした（design.md 13章「自動単体テストの対象は
core/・util」「UIの自動テストは初期版では持たない」）。

- `core/document/ReviewFlow` … incoming/conflict/rollback/baseline-replace の退避結合・確認済みの
  ベースライン更新・巻き戻し非活性判定・すべて確認済みのフリーズ/スキップ/一括取消・退避不能ガード。
  退避・ベースラインの実体格納と index破損復元・mark-and-sweep は sprint5 の SnapshotStore に委譲し、
  本層は経路（破壊的操作の前に退避を取る順序、種別の割当、可否判定）を組み立てる。
- `core/state` … state.json の version 付きシリアライズ・パース・アトミック書き込み・未知version安全側。
  index.json（sprint5 の index_io）と同一の version 規約（K2）に揃え、JSON は snapshot の json_lite を
  共用（依存を増やさない）。

## 変更ファイル一覧

新規（src/core/document/）:
- `src/core/document/review_flow.h` — `FileContentClass`（can_stash 判定）/`ReviewTarget`（freeze_hash 含む）/
  `AllConfirmedResult`/`ReviewFlow`（stash_incoming_before_save・stash_conflict_before_take・confirm・
  can_rollback・rollback・confirm_all・revert_all）
- `src/core/document/review_flow.cpp` — 退避不能ガード（can_stash=false は Unsupported でブロック）・
  4 種別退避の SnapshotStore 委譲・確認済みのディスク内容ベースライン更新・巻き戻し可否と退避先行・
  すべて確認済みのフリーズ（freeze_hash 不一致でスキップ）と baseline-replace 退避

新規（src/core/state/）:
- `src/core/state/state_types.h` — `WindowState`/`TabState`/`RecentItems`/`AppState`/`kStateVersion`/
  `kRecentLimit`（design.md 7章「state.json の主な内容」を型化）
- `src/core/state/state_io.h` / `state_io.cpp` — `serialize_state`/`parse_state`/`load_state`/`save_state`。
  未知version=Unsupported・破損/version欠落=Io・初回（不在）は空の現行version・最近20件クランプ・
  modeByType は {ext,mode} 配列で round-trip（json_lite にメンバ列挙 getter が無いため）

新規テスト:
- `tests/core/document/review_flow_test.cpp`（17 ケース）
- `tests/core/state/state_io_test.cpp`（11 ケース）

ビルド配線:
- `src/CMakeLists.txt` — core/document・core/state の .cpp/.h を pika_core に追加（sprint10 のコメント追記）
- `tests/CMakeLists.txt` — 2 テスト .cpp を pika_tests に追加

## must criteria 実装状況（すべて自動テスト化）

| # | criterion | 実装 / テスト | 状況 |
|---|-----------|--------------|------|
| 1 | 退避結合：incoming（保存衝突）・conflict（取り込み）・rollback（巻き戻し前）・baseline-replace（すべて確認済み）が発生し復元で原内容と一致 | `stash_incoming_before_save`/`stash_conflict_before_take`/`rollback`/`confirm_all` ＋ `ReviewFlowTest.IncomingStashSavesAndRestores`・`ConflictStashSavesAndRestores`・`RollbackStashesCurrentAndReturnsBaseline`・`BaselineReplaceStashSavesOldBaseline` | 充足 |
| 2 | index破損後に objects 走査から退避が復元できる | `ReviewFlowTest.StashesRecoverableAfterIndexCorruption`（ReviewFlow が積んだ退避を index.json 破損後に `recover_pending_stashes`→`restore_stash` で復元） | 充足 |
| 3 | 確認済み：ディスク内容でベースライン更新・未読解除。内容を持たない（10MB以上/画像）はハッシュベースラインのみ更新 | `confirm`（SnapshotStore.set_baseline 委譲）＋ `ReviewFlowTest.ConfirmUpdatesBaselineFromDiskAndClearsUnread`・`ConfirmLargeOrImageUpdatesHashBaselineOnly`（baseline_object 空・objects 不書き込み・復元は Unsupported） | 充足 |
| 4 | 巻き戻し：退避を取れないファイル（10MB以上/画像）では巻き戻しを提供しない（非活性）判定 | `can_rollback`/`rollback`（Unsupported）＋ `ReviewFlowTest.CanRollbackOnlyWhenBaselineObjectPresent`・`RollbackBlockedForLargeOrImage`（退避も積まれない） | 充足 |
| 5 | state.json/index.json が version を持ち、未知version は読まず・書き戻さず・再生成もせず安全側。全永続化が一時ファイル→rename のアトミック書き込み | state.json: `StateIoTest.UnknownNewerVersionFailsSafe`・`MissingVersionIsRejected`・`StateIoFsTest.SaveIsAtomicNoTempLeftBehind`・`UnknownVersionOnDiskIsNotRewritten`（未知version は書き戻されずディスク不変）。index.json は sprint5 で実装済み（`IndexIoTest.UnknownNewerVersionFailsSafe`・`SaveIsAtomicNoTempLeftBehind` が現存し全 PASS） | 充足 |

## should criteria 実装状況

| criterion | 実装 / テスト | 状況 |
|-----------|--------------|------|
| すべて確認済み：開始時未読集合フリーズ・処理中変化はスキップ（未読のまま）・baseline-replace を batchId で一括取消 | `confirm_all`（`freeze_hash` 不一致でスキップ）/`revert_all`（SnapshotStore.revert_batch 委譲）＋ `ReviewFlowTest.ConfirmAllSkipsFilesChangedDuringFreeze`・`ConfirmAllSkipsFileChangedSinceFreeze`・`ConfirmAllConfirmsWhenFreezeHashMatchesCurrent`・`RevertAllUndoesBatchBaselineReplace` | 充足 |
| 退避不能ガード：退避を取れないファイルへの破壊的操作が既定でブロックされる | `guard_stashable`（can_stash=false→Unsupported）＋ `ReviewFlowTest.IncomingStashBlockedForLargeOrImage`・`ConflictStashBlockedForSensitive`（平文 object が一切書かれないことも観測） | 充足 |

## テスト化できなかった criteria とその理由

なし。sprint10 の must 5 件・should 2 件はいずれも wx/WebView2/Win32 非依存の純ロジック（退避種別の割当・
確認済み/巻き戻しの可否・version 安全側・アトミック書き込みの .tmp 残存有無）であり、すべて gtest で観測
可能な挙動として検証した。

補足（本層のスコープ境界・後続申し送り。criteria に含まれない＝テスト化対象外）:
- 実 FS へのディスク反映（rollback のベースライン書き戻し・確認済みの保存促し・保存トークンによる自己
  イベント抑制）は DocumentController（UI/Win32 結線）の責務。本層は「巻き戻し先＝ベースライン内容」を
  返し、ディスクへの書き戻しは呼び出し側が続ける純オーケストレーション（design.md 5.3/5.4）。
- 確認済みの「差分計算時点のディスクスナップショット採用・確定直前の mtime/ハッシュ再照合」（E2）は、
  confirm_all では `freeze_hash` として API 化済み（不一致でスキップ）。単発 confirm の再照合は呼び出し
  側が confirm 前に行う（本層は受け取った content を信頼してベースライン化する純関数）。
- state.json の実ファイル配置（データルート解決・%LOCALAPPDATA% / portable）は sprint13 配布の責務。
  本層はパスを受け取り version/アトミック/安全側を担う（index_io と同型）。
- index.json 側の version 安全側・アトミックは sprint5 で実装済み・現存テスト全 PASS のため、本スプリント
  では state.json を index.json と同一規約で新規実装し、criterion5 を両ファイルで満たす形にした。

## 設計上の判断メモ（ドリフト追跡）

- **退避が先（データを失わない）**: rollback は「現ディスク内容を rollback 退避に保存してから」ベース
  ライン内容を返す（退避→破壊の順序を本層で固定）。退避が取れない対象は破壊前に Unsupported で止め、
  object を一切書かない（`store.objects().list_objects().empty()` で観測）。
- **退避不能ガードを 1 か所に集約**: `guard_stashable(cls)` を incoming/conflict/rollback で共有し、
  「can_stash = content_object_allowed && !sensitive」の判定を経路間で乖離させない。
- **confirm_all のフリーズ基準を freeze_hash で明示**: 「処理中に変化したファイルをスキップ」は、開始時
  点に見ていた内容（freeze_hash）と確定直前の現内容のハッシュ不一致で判定する。`ReviewTarget` に
  freeze_hash を持たせ、確定直前読みとフリーズ読みの食い違い（並行書き込み）を未確認内容のベースライン
  化から守る（要件8.3「ユーザーが見ていない内容をベースライン化しない」）。空のとき（単発経路）は
  チェックしない。
- **state.json は index.json と同一の version 規約**: parse は version 欠落/破損=Io、未知(>現行)=
  Unsupported、現行以下=受理。load は不在=空の現行version、破損/未知はエラーを伝播（呼び出し側が
  既定起動 or 安全側を選ぶ）。save は親ディレクトリ作成＋write_atomic（一時ファイル→rename）。
- **modeByType は配列で持つ**: json_lite（snapshot 由来）はオブジェクトのメンバ列挙 getter を持たない
  ため、拡張子→モードの順序付き写像を `{ext, mode}` オブジェクトの配列で表現し round-trip を保証した
  （json_lite を改変しない最小手）。

## 自己実行した verify の結果

合否判定の正本は run-dev が別途実行する結果。以下は自己確認。

- `cmake --preset x64-core-test` → exit 0（vcpkg manifest 解決・CMake 構成成立。新規 core/document・
  core/state ソースを pika_core へ配線して再構成）
- `ctest --preset x64-core-test` → exit 0。100% tests passed（pika_tests_build / pika_tests の 2 テスト）
  - 全体: 367 tests PASS（sprint9 末 365 → 本スプリントで +2 スイートぶん増、ReviewFlowTest 17 ＋
    StateIoTest 6 ＋ StateIoFsTest 5 = 28 ケースを新規追加し全 PASS）
  - 既存スイート（diff/render/watcher/snapshot/search/settings/workspace/ipc/util）に回帰なし

途中で 1 件のビルドエラー（review_flow_test.cpp が `pika::util::write_atomic` を使うのに
`util/atomic_file.h` 未 include）を検出し、include 追加で解消。テストの緩和・スキップ・無効化はしていない
（criterion の観測対象＝退避復元・version 安全側・アトミック性を保ったまま include を補った）。

編集直後 hook（clang-format --dry-run --Werror）は全 C++ ファイルで素通し（整形済み）。

DONE: C:/dev/pika_editor/dev/turns/turn-10-1-generator.md
