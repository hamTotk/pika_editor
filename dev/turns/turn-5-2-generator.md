# turn 5-2 generator report (sprint 5 / iteration 2)

sprint 5 goal: core/snapshot のベースライン・退避の保存/復元と容量管理。データを失わない原則
（index 破損時の objects 走査復元を含む）を gtest で検証する。

本ターンは iteration 2（修正ターン）。turn-5-1-eval.json の `feedback_structured` を
high → medium → low の順に対応した。前ターンは d_verify=100 / d_review=100 だが
**d_accept=0（must 1 件未充足）/ d_quality=72** で不合格。

## 対応サマリ（feedback 順）

### HIGH 1（must 未充足・最優先原則の毀損）: add_stash の LRU が 14日保護を無視
- 事象: `add_stash` のインライン per-file LRU（旧 snapshot_store.cpp:162-179）が `is_protected` を
  確認せず先頭から無条件 erase。同一ファイルに11件以上の退避が14日以内に積まれると、
  未復元・14日以内の保護退避が静かに削除され「データを失わない＝退避が最後の砦」に反した。
  `enforce_capacity` 側 LRU は保護を尊重しており、二経路が乖離していた。
- 修正: per-file LRU を **単一実装 `SnapshotStore::apply_per_file_lru(entry, now)`** に集約した
  （header private に宣言）。保護判定 `!restored && (now - time) <= protect_seconds` を満たす退避は
  baseline-replace と同様に10件枠の削除対象から除外する。`add_stash` は新規退避の生成時刻 `time` を
  保護判定の基準時として呼び出し、`enforce_capacity` は `now` を渡す。これで両経路が同一規則を共有し
  乖離が解消。保護退避は11件目以降も残る。

### HIGH 2（テスト品質・被覆漏れ）: LRU×14日保護の相互作用を観測する gtest 追加
- `AddStashLruKeepsProtectedBeyondTen`: 11件すべて未復元・14日以内 → 10件枠を超えても1件も落ちず
  最古も復元できることを観測（add_stash 経路）。
- `AddStashLruDropsUnprotectedButKeepsProtected`: 非保護の古い退避は LRU で落ち、保護退避8件は枠超過
  でも全て残る混在ケース（保護8＋非保護残2＝10）。
- `EnforceCapacityLruKeepsProtectedBeyondTen`: enforce_capacity 経路でも保護退避11件が削られないこと
  を観測（二経路非乖離の確認）。

### HIGH 3（security・パストラバーサル多層防御）: index.json 由来 hash の許可リスト検証
- `ObjectStore::is_valid_hash`（static）を追加し、XXH3-64 = 16 桁の小文字 [0-9a-f] のみ通す。
  `get`/`contains`/`remove`（＝外部由来 hash がパス合成に届く読み取り・削除の入口）で、合成前に検証し
  不正 hash は object 不在（`get`=NotFound / `contains`=false / `remove`=no-op）として扱う。
  put/put_stash の hash は内容から内部計算するため検証不要（常に妥当）。
- テスト: `ObjectStoreRejectsMalformedHash`（".."・区切り・大文字・長さ違い・空を拒否）、
  `RestoreStashWithMalformedHashIsNotFound`、`CraftedIndexHashDoesNotEscapeObjectsDir`
  （objects 外のファイルが sweep/restore で消えない/読まれない）。

### HIGH 4（performance・容量GC の O(V·R) syscall）: object サイズ事前集計＋参照カウント
- `enforce_capacity` の容量GC を、1件削除ごとに全 object を `fs::file_size` 再計測する方式から、
  **ループ前に object→size を1度だけ集計し、参照カウントで合計を維持**する方式へ変更。退避削除時は
  該当 object の参照カウントを減算し、0（最後の参照消滅）になったときだけ合計から減算する
  （重複排除を尊重）。容量GC ループ中の再 syscall を排し「固まらない」を守る。
  既存 `ByteGcEvictsOldestUnprotectedButKeepsProtected` で回帰なしを確認。

### MEDIUM 1（security・DoS）: json_lite の再帰深さ上限
- `kMaxDepth=64` を導入。`parse_value` でネスト1段ごとに深さを進め、超過時は破損扱い（false）で打ち切る
  （上位は退避走査の復元経路へフォールバック）。テスト `JsonParserRejectsDeeplyNestedInput`。

### MEDIUM 2（security・解凍爆弾）: decompress の展開後サイズ上限
- `kMaxDecompressedSize=40MB`（内容 object 対象の編集可能テキスト10MB×4倍の余裕）を導入。`out` 確保前に
  `ZSTD_getFrameContentSize` の宣言値を検査し、超過フレームは Io エラーで弾く。
  テスト `DecompressRejectsOversizedFrame`。

### MEDIUM 4（ux・復元導線）: recover_pending_stashes の決定的順序
- `scan_recoverable_stashes` の結果を `time` 降順 → `index_gen` 降順 → `object_hash` 昇順で安定ソートし、
  directory_iterator の不定順に依存しない決定的な復元待ち一覧順序を本層で保証した。

### MEDIUM 3（performance・計算量）: 申し送り（本ターンでは未対応）
- `SnapshotIndex::find` の線形走査（N件一括 set_baseline で O(n^2)）と即時 sweep のコストは medium 指摘。
  find のハッシュマップ化は `SnapshotIndex` 型（snapshot_types.h）の構造変更を伴い、本型は後続スプリント
  （workspace/diff/結合）のテストからも参照されるため、修正ターンでの型再設計はスコープ拡大リスクが高い。
  なお add_stash の即時 sweep は本ターンで「LRU が実際に退避を落としたときだけ走る」よう条件化
  （`stash.size() != before`）し、保護尊重で発火頻度自体が大きく下がった。find のマップ化は性能スプリント
  または workspace 結合スプリントで型と併せて扱うのが妥当と判断し、evaluator の判断に委ねる。

### LOW（申し送り）
- low 3 件（is_sensitive_default の結線は後続呼び出し側／復元失敗メッセージの「次の一手」は上位 UI 責務／
  kMetaSuffix の微最適化）はいずれも本層外または誤差レベルのため、記録のみで未対応。eval の指摘どおり
  後続スプリントの結線・UI 翻訳で扱う。

## 既存テストの修正（基準改竄ではない・根拠を明示し評価に委ねる）

iteration 1 の以下2テストは **14日保護を無視していた旧実装を前提に書かれており**、本ターンで充足を
求められた must criterion『未復元かつ14日以内の退避を保護対象から削除しない』と**直接矛盾**していた
（全件を直近・未復元の時刻で積むため、保護を尊重すると全件が保護対象になり「10件まで削る」ことが
criterion 違反になる）。テストの緩和・skip・無効化ではなく、**LRU の10件枠が観測可能になる
非保護（＝14日より古い）退避を含む構成へ修正**し、LRU の上限挙動の検証は維持した:

- `PerFileStashLruKeepsLatestTen`: 15件を30日間隔で投入。最新1件のみ保護、残り14件は非保護。LRU が
  非保護退避を10件枠に収め最新10件が残ることを検証（上限挙動は維持。アサーション緩和なし）。
- `BaselineReplaceNotCountedInPerFileLru`: 12件の Conflict を30日間隔で投入し、baseline-replace が
  10件枠の外であること（非バッチ10件＋baseline-replace1件＝11件）を検証。

判断根拠: criterion は LRU を「保護機構」として明示列挙しており、保護より10件ハードキャップを優先する
旧テスト期待値は spec 違反。保護を尊重しつつ LRU 上限を観測する唯一の方法が非保護退避の混在であるため
本修正とした。誤りと考える場合に備え根拠を本報告に残す（ref-dev「テストが誤りなら根拠を書いて評価に委ねる」）。

## 変更ファイル一覧

- `src/core/snapshot/snapshot_store.h` — `apply_per_file_lru`（private）宣言を追加。
- `src/core/snapshot/snapshot_store.cpp` — LRU を `apply_per_file_lru` に集約し add_stash/enforce_capacity
  双方から呼ぶ（保護尊重で二経路の乖離解消）。容量GC を object サイズ事前集計＋参照カウント方式へ。
  `#include <map>` 追加。
- `src/core/snapshot/object_store.h` — `is_valid_hash`（static）宣言を追加。
- `src/core/snapshot/object_store.cpp` — `is_valid_hash` 実装。get/contains/remove でパス合成前に検証。
  `scan_recoverable_stashes` の結果を決定的順序にソート。`#include <algorithm>` 追加。
- `src/core/snapshot/json_lite.cpp` — 再帰下降パーサに深さ上限 `kMaxDepth` を導入（DoS 耐性）。
- `src/core/snapshot/compression.cpp` — decompress に展開後サイズ上限 `kMaxDecompressedSize`（解凍爆弾）。
- `tests/core/snapshot/snapshot_store_test.cpp` — LRU×14日保護の相互作用3件・security3件・robustness2件の
  gtest を追加。spec 矛盾の既存2テスト（PerFileStashLruKeepsLatestTen /
  BaselineReplaceNotCountedInPerFileLru）を保護尊重前提へ修正（上限挙動の検証は維持）。

## must / should 実装状況

must:
1. 保存→復元の同一性 — 充足（BaselineSaveRestoreIdentity / BaselineRoundTripsAfterPersist。前ターン維持）
2. 退避フロー4種別 — 充足（AllStashKindsSaveAndRestore。前ターン維持）
3. index 破損復元 — 充足（RecoversStashesAfterIndexCorruption。前ターン維持）
4. 機密はハッシュのみ — 充足（SensitiveFileRecordsHashOnly。平文 object 不在も検証。前ターン維持）
5. 容量管理（LRU・容量GC・90日GC が未復元14日以内を保護から削除しない） — **本ターンで充足化**。
   add_stash の LRU を保護尊重に修正し、LRU×保護の相互作用を3テストで観測
   （AddStashLruKeepsProtectedBeyondTen / AddStashLruDropsUnprotectedButKeepsProtected /
   EnforceCapacityLruKeepsProtectedBeyondTen）。90日GC・容量GC の保護は前ターンのテストを維持。
6. mark-and-sweep の誤削除防止 — 充足（SweepKeepsSharedObjectReferencedElsewhere /
   SweepKeepsBaselineObjectSharedWithStash。前ターン維持）

should:
- ワークスペース非汚染 — 充足（DoesNotWriteInsideWorkspaceFolder。前ターン維持）
- 起動時未読プレスクリーン — 未充足（前ターン同様。baseline_mtime/size フィールドのみ存在し、
  プレスクリーン比較ロジックは workspace/起動時判定スプリント（sprint 8/10 系）で実装予定。本層では
  データ基盤のみ）。
- ACL 本人のみ — 充足（secure_dir の owner-only SDDL。前ターン維持）

## テスト化できなかった criterion とその理由
- なし（must は全件 gtest 化済み）。should の「起動時未読プレスクリーン」は本スプリントの責務外
  （後続スプリントの起動時判定で結線）のため本層では未テスト。

## 自己実行した verify 結果（合否の正本は run-dev の別途実行）

- `cmake --preset x64-core-test` → 構成成立（vcpkg manifest 解決済み、Configuring/Generating done）。
- `cmake --build --preset x64-core-test` → 警告ゼロでビルド成功（/W4・警告エラー扱い下）。
- `ctest --preset x64-core-test` → **100% tests passed, 0 failed out of 2**
  （pika_tests_build / pika_tests。全 217 テスト中 SnapshotStoreTest 25件を含め全 PASS）。
- 編集直後 hook（clang-format --dry-run --Werror）→ 全編集ファイルでクリーン。

DONE: C:/dev/pika_editor/dev/turns/turn-5-2-generator.md
