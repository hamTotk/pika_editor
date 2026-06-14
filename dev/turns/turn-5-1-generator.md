# turn-5-1 generator report — sprint 5: core/snapshot

## 目標（再注入）

sprint 5 goal: core/snapshot のベースライン・退避の保存/復元と容量管理を実装し、データを失わない原則
（index破損時の objects 走査復元を含む）を gtest で検証する。中心体験の差分・確認済みの永続基盤を固める。
spec.md「機能一覧」snapshot 項・design.md 7章/9章・要件9章を正典として実装した。UI/wx 非依存・
実 FS は gtest のテンポラリのデータルートで検証（design.md 13章 F2/F6）。

## 変更ファイル一覧

### 実装（src/core/snapshot/ 新規）
- `snapshot_types.h` / `.cpp` — index.json のメモリモデル（IndexEntry・StashEntry・StashKind 4種・
  SnapshotIndex）。kind 文字列化・LF正規化ハッシュ前提の baselineHash。
- `compression.h` / `.cpp` — zstd 圧縮/復元（ロスレス・破損フレームは Io エラー）。`zstd::libzstd`。
- `json_lite.h` / `.cpp` — index.json / サイドカー専用の最小 JSON（外部入力ではなく pika 自身が書く
  固定スキーマのみ。依存追加を避ける）。UTF-8 はそのまま、`\uXXXX`・サロゲートにも対応。
- `object_store.h` / `.cpp` — content-addressed objects（XXH3-64 hex 名・重複排除・zstd）。退避は
  自己記述サイドカー `<hash>.meta`（relPath/kind/time/indexGen/batchId）併記。`scan_recoverable_stashes`
  が objects 走査だけで復元待ち退避一覧を再構築（D1）。
- `index_io.h` / `.cpp` — index.json の serialize/parse・load/save。version 付き・未知version は
  Unsupported で安全側・破損は Io・アトミック書き込み（util/atomic_file 経由）。
- `sensitive.h` / `.cpp` — 機密ファイル判定（`.env*`・`*.env`・`*.key`・`*.pem`・`*secret*`、大小無視
  glob）。一致ファイルは内容 object を保存しない。
- `secure_dir.h` / `.cpp` — 本人のみ ACL（SDDL `D:P(A;OICI;FA;;;<SID>)`）でのディレクトリ作成。
  ACL 設定失敗時も継承 ACL で作成を続行（保全＞秘匿。設計原則1）。
- `snapshot_store.h` / `.cpp` — 中核。wsKey（workspace_key / file_key）・set_baseline/restore_baseline・
  add_stash（per-file 最新10件 LRU・baseline-replace は別バッチ枠）・restore_stash・revert_batch・
  enforce_capacity（LRU→容量GC500MB→90日GC、未復元14日保護を侵さない）・
  sweep_unreferenced_objects（mark-and-sweep）・purge・recover_pending_stashes。

### テスト（tests/core/snapshot/ 新規。gtest 41件）
- `compression_test.cpp`（6件）/ `sensitive_test.cpp`（5件）/ `index_io_test.cpp`（8件）/
  `object_store_test.cpp`（5件）/ `snapshot_store_test.cpp`（17件）。

### ビルド構成
- `src/CMakeLists.txt` — snapshot 8 モジュールを pika_core に追加。`find_package(zstd)`・
  `zstd::libzstd`（PRIVATE、内部実装詳細）・`advapi32`（ACL API）をリンク。
- `tests/CMakeLists.txt` — snapshot テスト 5 ファイルを pika_tests に追加。

## must criteria 実装状況（すべて gtest 化済み）

| must | 実装 / 検証テスト |
|---|---|
| 保存→復元の同一性（zstd・objects 内容ハッシュ名） | `ObjectStore::put/get`・`SnapshotStore::set_baseline/restore_baseline`。`CompressionTest.RoundTrips*`・`ObjectStoreTest.PutGetRoundTrip/DeduplicatesSameContent`・`SnapshotStoreTest.BaselineSaveRestoreIdentity/BaselineRoundTripsAfterPersist` |
| 退避フロー conflict/incoming/rollback/baseline-replace の保存・復元 | `SnapshotStore::add_stash/restore_stash`。`SnapshotStoreTest.AllStashKindsSaveAndRestore`（4種を網羅し復元一致を観測） |
| index破損復元（objects 自己記述メタ走査で復元待ち一覧） | `ObjectStore::scan_recoverable_stashes`・`SnapshotStore::recover_pending_stashes`。`SnapshotStoreTest.RecoversStashesAfterIndexCorruption`（破損後 load=Io、走査復元で内容一致）・`ObjectStoreTest.ScanRecoversStashFromSidecar/ScanSkipsSidecarWithMissingObject` |
| 機密ファイルは内容を保存せず baselineHash のみ | `SnapshotStore::set_baseline`（sensitive 時 object 不保存）・`sensitive.*`。`SnapshotStoreTest.SensitiveFileRecordsHashOnly`（objects 空・復元 Unsupported）・`SensitiveTest.*` |
| 容量管理: 最新10件 LRU・容量GC500MB・90日GC・未復元14日保護 | `SnapshotStore::add_stash`(LRU)・`enforce_capacity`。`SnapshotStoreTest.PerFileStashLruKeepsLatestTen/AgeGcRemovesOldButProtectsRecentUnrestored/AgeGcRemovesRestoredEvenIfRecent/ByteGcEvictsOldestUnprotectedButKeepsProtected` |
| object 物理削除が mark-and-sweep で共有実体を誤削除しない | `SnapshotStore::sweep_unreferenced_objects`。`SnapshotStoreTest.SweepKeepsSharedObjectReferencedElsewhere/SweepKeepsBaselineObjectSharedWithStash` |

## should criteria 実装状況

| should | 実装 / 検証 |
|---|---|
| ワークスペース内に管理ファイル(.pika 等)を一切作らない | 保存先は snapshots\<wsKey>\ のみ。`SnapshotStoreTest.DoesNotWriteInsideWorkspaceFolder`（ワークスペース実体フォルダのエントリ数 0 を観測）で gtest 確認 |
| 起動時未読判定が mtime+サイズ→不一致のみハッシュ | IndexEntry に baseline_mtime/baseline_size を保持し set_baseline で記録（プレスクリーンの基礎）。本スプリントは snapshot 永続基盤までを範囲とし、起動時走査の結合（WorkspaceModel 連携）は sprint 8 の workspace で実装する想定（未読集合は workspace の責務）。プレスクリーン用フィールドの保持・往復は `IndexIoTest.SerializeParseRoundTrip` で観測 |
| snapshots/index/objects が本人のみ ACL で作成される方針 | `secure_dir.cpp`（SDDL `D:P(A;OICI;FA;;;<SID>)`）を save/set_baseline/add_stash の各書き込み前に呼ぶ。`SnapshotStore::save/add_stash` がディレクトリ作成に使用 |

## テスト化できなかった criteria とその理由

- should「起動時未読判定が mtime+サイズプレスクリーン→不一致のみハッシュ比較で動く」: 本スプリントは
  snapshot の保存/復元・容量管理・破損復元（must）に範囲を絞った。未読集合・プレスクリーン走査は
  WorkspaceModel（sprint 8 workspace, depends_on=[3,5]）の責務であり、snapshot 側は判定に使う
  メタ（baseline_mtime/baseline_size）の保持・往復までを提供する。観測は `IndexIoTest` の往復で担保。
  挙動全体の gtest は sprint 8 で workspace と結合して実装する。
- should「ACL が本人のみで作成される」を gtest で直接アサートしていない: ACL の実効権限検証は
  AccessCheck 相当のコードを要し、CI/管理者権限・実行ユーザー依存で不安定になりうるため、
  方針の実装（secure_dir.cpp の SDDL 構築・呼び出し配線）に留めた。ディレクトリが実際に作成され
  読み書きできることは多数のテスト（保存/復元）が間接的に観測している。

## 自己実行した verify の結果（sprints.json sprint5 verify）

- `cmake --preset x64-core-test` — exit 0。vcpkg manifest が zstd を解決、CMake 構成成立（warning なし）。
- `ctest --preset x64-core-test` — exit 0。`pika_tests_build`（/W4・/WX で警告ゼロでビルド成功）→
  `pika_tests` の順で 2/2 Passed。gtest 全体 209 tests passed（うち snapshot 関連 41 件）。

注: 合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

## 設計上の判断メモ（ドリフト追跡）

- JSON ライブラリを依存追加せず `json_lite` を内製した（CLAUDE.md「軽い」「依存追加は天秤」）。
  対象は pika 自身が書く固定スキーマのみで外部入力ではない。IPC（sprint7）の外部 JSON 検証とは
  用途・脅威モデルが異なるため別実装とした。
- `sensitive.cpp` の既定パターンに `.env*` を追加した（要件9.1 の「.env 等」に含まれる実運用派生
  .env.local 等をデータ最小化の強化側として網羅）。テストの期待に合わせて緩めたのではなく、機密判定を
  保守的（広く検出）側へ倒す変更であり、データを失わない/漏らさない原則に整合する。
- mtime は Win32 FILETIME（100ns）系と Unix epoch 秒（退避時刻）で別系統を使う。退避時刻は LRU/GC の
  比較にのみ使うため秒精度で十分とし、呼び出し側（後続スプリントの DocumentController）が time を渡す
  設計とした（テストでは決定論的な固定値を注入して観測）。

DONE: C:/dev/pika_editor/dev/turns/turn-5-1-generator.md
