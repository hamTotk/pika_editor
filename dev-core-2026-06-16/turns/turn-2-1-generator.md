# turn-2-1 generator report — sprint 2（util 基盤）

- sprint_id: 2 / iteration: 1 / feedback: null（初回ターン）
- goal: util の基盤（エンコーディング判定・往復、ハッシュ、アトミック書き込み、Result<T>、
  TaskRunner）が UI 非依存で実装され gtest で検証される。コア全体が依存する土台を決定論的に固める。
- spec 再注入: 文字列は std::string（UTF-8）に統一し Win32 境界で UTF-16 変換（design.md 2章）／
  コア公開 API は Result<T>・例外はモジュール内部に閉じる（15章）／util は wx 非依存（13章）／
  診断ログは内容を書かない（12章）。本実装はこれらに沿う。

## 変更ファイル一覧（すべて project_dir 内）

新規作成:

- `C:/dev/pika_editor/src/util/result.h` — Result<T>／Result<void>・ErrorCode・ErrorInfo
- `C:/dev/pika_editor/src/util/encoding.h` / `encoding.cpp` — Encoding/Newline/DecodedText、
  decode_auto・decode_as・encode・can_encode・detect_newline（Win32 CP932/UTF-16 往復）
- `C:/dev/pika_editor/src/util/atomic_file.h` / `atomic_file.cpp` — write_atomic（一時ファイル→
  ReplaceFile/MoveFileEx）・read_all
- `C:/dev/pika_editor/src/util/task_runner.h` / `task_runner.cpp` — TaskRunner（2〜4 本プール、
  submit→future）
- `C:/dev/pika_editor/src/util/logger.h` / `logger.cpp` — Logger（op/path/detail のみ・内容を書かない）
- `C:/dev/pika_editor/tests/util/result_test.cpp`（6 ケース）
- `C:/dev/pika_editor/tests/util/encoding_test.cpp`（BOM 判定・候補順・往復 10 パラメタ・表現可能性 ほか）
- `C:/dev/pika_editor/tests/util/atomic_file_test.cpp`（8 ケース・テンポラリ FS）
- `C:/dev/pika_editor/tests/util/task_runner_test.cpp`（5 ケース）
- `C:/dev/pika_editor/tests/util/logger_test.cpp`（5 ケース）

既存ファイルの編集:

- `C:/dev/pika_editor/src/util/hash.h` / `hash.cpp` — LF 正規化ハッシュ（xxh3_64_lf・xxh3_64_lf_hex）を追加。
  既存の xxh3_64/xxh3_64_hex は据え置き、hex 化を to_hex16 に共通化
- `C:/dev/pika_editor/tests/util/hash_test.cpp` — Xxh3LfHash 4 ケースを追記（既存 5 ケースは温存）
- `C:/dev/pika_editor/src/CMakeLists.txt` — pika_core に新規 util ソースを追加
- `C:/dev/pika_editor/tests/CMakeLists.txt` — pika_tests に新規テストソースを追加

Read のみ（書き換えていない）: `dev/spec.md`・`dev/sprints.json`・`dev/turns/*-eval.json`・
`~/.claude/skills/ref-dev/`・`docs/`・`.clang-format`・`.github/`・`CMakePresets.json`・`vcpkg.json`。

## must criteria の実装状況（すべて充足・自動テスト化）

1. **EncodingDetector: BOM 最優先＋BOMなしは UTF-8→Shift_JIS 順、UTF-8/BOM/UTF-16/Shift_JIS ×
   LF/CRLF の往復維持** → 充足。
   `decode_auto` は (1) UTF-8 BOM(EF BB BF)/UTF-16 LE(FF FE)/BE(FE FF) を最優先、(2) BOM なしは
   自前 `is_valid_utf8`（過長符号化・サロゲート・範囲外を弾く）で UTF-8 妥当性 → 不可なら CP932
   デコード（MB_ERR_INVALID_CHARS）、(3) いずれも不可なら UTF-8 lossy で開く（最後の砦・失敗にしない）。
   テスト: `EncodingDetect.*`（BOM 3 種・UTF-8 採用・SJIS フォールバック・ASCII）と
   `AllEncodings/EncodingRoundTrip`（4 エンコーディング × LF/CRLF＝10 パラメタ）で encoding・has_bom・
   newline・content の往復一致と再エンコードのバイト安定性を検証。`EncodingRoundTrip2` で auto 判定→
   再エンコードの原バイト一致も確認。

2. **書き出し時の表現可能性チェック: Shift_JIS 不能文字（絵文字等）で保存中断エラー** → 充足。
   `encode(.., ShiftJis, ..)` は WideCharToMultiByte の `lpUsedDefaultChar` 置換を検出し、
   発生時は ErrorCode::Encoding を返して保存を中断（無確認の文字欠落を起こさない。要件5.2）。
   UTF-8/UTF-16 は全 Unicode 表現可のため検査不要（`can_encode` も true）。
   テスト: `EncodingRepresentable.ShiftJisRejectsEmoji`（U+1F600 で is_err）/`ShiftJisAcceptsRepresentable`
   /`Utf8AndUtf16AlwaysRepresentable`。

3. **ハッシュ: LF 正規化後 XXH3 が CRLF/LF のみ異なる同一内容で一致** → 充足。
   `xxh3_64_lf` は CRLF を LF へ畳んでから XXH3（CR 単独は畳まない）。
   テスト: `Xxh3LfHash.CrlfAndLfOnlyDifferenceMatches`（一致）/`MixedNewlinesNormalizeToLf`（混在も一致）
   /`ContentDifferenceStillDiffers`（内容差は残る）/`LoneCrIsNotNewlineNormalized`（CR 単独は別物）。

4. **AtomicFile: 一時ファイル→rename のアトミック書き込み・書き込み途中でも元ファイルが破損しない** →
   充足。`write_atomic` は path と同一ディレクトリに `*.pika-<pid>-<tick>.tmp` を作り全内容を書いて
   FlushFileBuffers → 既存ありは ReplaceFileW（属性/ACL 維持・アトミック）・無しは MoveFileExW。
   失敗時は一時ファイルを削除し最終パスの旧内容を変更しない。
   テスト（テンポラリフォルダ使用）: 新規書込・上書き・NUL 含むバイナリ往復・空内容・成功後の一時
   ファイル残存ゼロ・**失敗時に別の既存ファイルが無傷**（`FailedWriteDoesNotCorruptExisting`）・
   欠損読みは NotFound。

5. **コア公開 API が Result<T> 方式で、失敗時に例外を投げずエラー値を返す** → 充足。
   `Result<T>`（std::variant ベース・既定構築禁止）と `Result<void>` を導入。本スプリントの公開 API
   （encode/decode_*・write_atomic/read_all）はすべて Result を返す。
   テスト: `ResultT.*`（ok 値・err code/message・move-only 値・**失敗経路で例外が飛ばない**）/`ResultVoid.OkAndErr`。
   `atomic_file`・`encoding` の各失敗テストも「is_err＋code 照合」で例外非送出を観測。

## should criteria の実装状況

1. **TaskRunner（2〜4 本のワーカープール）でタスク投入と完了取得** → 充足。
   `TaskRunner::submit` が packaged_task→future を返し、worker_count は [2,4] にクランプ。
   テスト: クランプ境界（1→2 / 9→4 / 既定）・future 結果取得・200 タスク並行・タスク例外は future へ
   伝播しプールは生存・void タスク。

2. **Logger がファイル内容を書かずパス・操作・エラーのみ記録する方針** → 充足。
   API は op/path/detail（非機密メタ）だけを受け取り、ファイル内容を渡す経路を型として持たない
   （構造的に内容混入を防ぐ）。sink 差し替え可・sink 無しでも安全。
   テスト: `LoggerTest.*`（整形・detail 省略・sink 受信・内容チャネル不在・sink なし安全）。

## テスト化できなかった criteria とその理由

- must は 5 件すべて自動テスト化済み（テスト化漏れなし）。should 2 件も自動テスト化済み。
- ACL（本人のみ）の検証は sprint 5（snapshot のデータルート ACL）の範囲であり、本スプリントの
  AtomicFile は ReplaceFile による属性/ACL 維持の経路のみ実装（既存ファイルの ACL を引き継ぐ）。
  新規作成時の明示 ACL 付与は sprint 5 で扱う（YAGNI・spec のスプリント割当に従う）。

## verify 自己実行の結果（合否の正本は run-dev の実行）

sprints.json sprint 2 の verify（`ctest --preset x64-core-test`）に加え、構成段も実行:

```
cmake --preset x64-core-test   -> exit 0（vcpkg manifest 解決＋構成 done）
ctest  --preset x64-core-test  -> exit 0（2/2 PASS: pika_tests_build フィクスチャ＋pika_tests）
```

`pika_tests` 内の gtest は **53 ケース全 PASS**（直接実行で `[PASSED] 53 tests` を確認）。
内訳: 既存 hash 5 ＋ 新規 Xxh3LfHash 4 ＋ Result 6 ＋ Encoding（Detect6/Newline1/RoundTrip 10+1/
Representable3/Reopen1）＋ AtomicFile 8 ＋ TaskRunner 5 ＋ Logger 5。

### /W4・/WX 警告ゼロの確認

新規 util ソース 5 本を強制再コンパイル（touch 後ビルド）し、`warning C####`/`error C####` が
ゼロであることを確認（/WX により警告があればビルド失敗するため、ビルド成功＝警告ゼロが担保）。
`pika_core.lib`・`pika_tests.exe` 生成成功。

### 設計判断（評価者向けの根拠）

- **Shift_JIS は Win32 CP932（MultiByteToWideChar/WideCharToMultiByte）で往復**する。uchardet 等の
  依存追加はせず（design.md「uchardet 同梱は見送る」）、判定は自前のデコード妥当性検査＋CP932 デコード可否。
  対応環境は Windows 11 x64 のみ（CLAUDE.md）のため Win32 依存は util 層の許容範囲（UI 非依存は維持・
  wx 非依存。テストは Windows 上で決定論的に通る）。
- **保存時の改行は変換しない**（content の改行をそのまま出力）。混在改行も維持する（要件5.2）。
  newline は記録専用（Mixed/None も含む）で、保存バイト列の生成には関与しない。
- **LF 正規化は CRLF のみを畳む**（CR 単独は畳まない）。未読・差分照合の土台として
  「改行コードのみの差を出さない」要件8章に合わせ、後続 sprint 6（diff）と整合する。
- **Result<T> は variant ベース・既定構築禁止**。`Result<void>` は値を持たない経路用に特殊化。
  これにより「未初期化の曖昧な Result」を構造的に排除する。
- テストの緩和・スキップ・無効化は一切していない。各 must を観測可能な挙動（往復一致・is_err＋code・
  ハッシュ一致/不一致・一時ファイル残存ゼロ・別ファイル無傷）で検証した。

DONE: C:/dev/pika_editor/dev/turns/turn-2-1-generator.md
