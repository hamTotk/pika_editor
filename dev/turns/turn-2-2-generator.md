# turn 2-2 generator report

- sprint_id: 2 / iteration: 2
- goal: util の基盤（エンコーディング判定・往復・ハッシュ・アトミック書き込み・Result<T>・TaskRunner）が
  UI 非依存で実装され gtest で検証される。コア全体が依存する土台を決定論的に固める。
- feedback 入力: `C:/dev/pika_editor/dev/turns/turn-2-1-eval.json`（iteration 1 の eval）

## 前ターン feedback への対応（high → medium → low の順）

iteration 1 は d_verify=100 だが d_accept=0・d_review=0（data critical 1 件）・d_quality=72 で不合格。
feedback_structured に沿って以下を修正した。

### [HIGH-1] エンコーディング往復 / 不可逆データ損失（d_review critical / must#2 の構造的抜け穴）

- 指摘: `src/util/encoding.cpp` の `wide_to_cp932` が `WideCharToMultiByte(kCp932, 0, ...)` を
  フラグ 0（ベストフィット変換有効）で呼び、`WC_NO_BEST_FIT_CHARS` を付けていなかった。CP932 に
  真のマッピングを持たない一部 Unicode 文字がベストフィットで別バイト列へ静かに化け、このとき
  `lpUsedDefaultChar` が立たないため `can_encode`/`encode` の表現可能性チェック（`!used_default`）を
  すり抜け、保存中断されず不可逆破壊が起きていた。
- 修正: サイズ取得・本変換の両方の `WideCharToMultiByte(kCp932, ...)` に `WC_NO_BEST_FIT_CHARS` を
  付与（`src/util/encoding.cpp` の `wide_to_cp932`）。未マップ文字は既定文字へ落ち `used_default` が
  確実に立つため、表現不能文字が保存中断される（化けたバイトを書き出さない）。サイズ取得呼び出しでは
  Win32 仕様により `lpUsedDefaultChar`/`lpDefaultChar` を渡せないため本変換側でのみ受け取っている。
- 補足（評価への申し送り）: eval は例として「波ダッシュ U+FF5E→0x8160」を挙げていたが、実機の
  Windows CP932 では **U+FF5E 全角チルダは 0x8160 に真にマップされ往復安定（表現可能）**だった
  （波ダッシュ問題：Microsoft CP932 は 0x8160 を U+301C ではなく U+FF5E にマップする）。同様に
  **U+FF0D 全角ハイフンマイナスも 0x817C に真のエントリを持ち表現可能**。実際にベストフィット対象
  （未マップ＝used_default が立つ）として実機で確認できたのは **U+301C 波ダッシュ** と
  **U+2212 マイナス記号**。テストはこの実機事実に合わせて踏む文字を選定した（下記）。実装の修正方針
  （WC_NO_BEST_FIT_CHARS 付与）は feedback どおりで、これにより「真に未マップな文字は中断、真に
  マップされる文字は通す」が成立する。

### [HIGH-2] アトミック書き込み / クラッシュ耐性（must#4 を成功側に倒していた）

- 指摘: `src/util/atomic_file.cpp` の `write_all_to` が `FlushFileBuffers(h)` の戻り値を検査せず、
  失敗しても rename へ進んでいた。ディスク確定保証なしで最終パスへ昇格し、直後のクラッシュ/電源断で
  旧新両方を失いうる。
- 修正: `FlushFileBuffers` の戻り値を検査し、失敗時は `CloseHandle` 後に未確定の tmp を `DeleteFileW`
  で削除し `ErrorCode::Io` を返す（rename へ進ませない）。最終パスは無変更のまま保たれる。

### [MEDIUM] エンコーディング往復 / 自己保存抑制の整合（sprint3 連結前の方針明確化）

- 指摘: decode 経路が原バイトを保持せず保存時に再生成するため、bijective でない文字の無編集保存で
  ディスクバイトが変わりうる。sprint3 の自己保存抑制（保存後ハッシュ一致が主条件）で外部変更誤認/
  未読化の温床になる。原バイト保持か往復不変文字集合への限定方針を明確化すべき。
- 対応: 「往復不変な文字集合への限定」方針を採用し、契約面である `src/util/encoding.h` 冒頭の方針
  コメントに明文化した。HIGH-1 の WC_NO_BEST_FIT_CHARS により、encode が成功する文字集合は
  decode→encode でディスクバイトが安定する（真に未マップな文字は encode 中断）。これにより無編集
  保存でバイトが変わらず、sprint3 の自己保存抑制（保存後ハッシュ一致）の前提が満たされる。原バイト
  そのものは保持しない（UTF-8 を正本に保存時再生成）が、許可集合が往復不変であるためハッシュ整合は
  保たれる。回帰テスト `ShiftJisAcceptsTrueCp932Symbols` が decode→encode のバイト安定を観測する。

### [LOW] アトミック置換 / 競合（TOCTOU）

- 指摘: `GetFileAttributesW` 判定と `ReplaceFile`/`MoveFileEx` 置換の間に TOCTOU 窓があり、判定後に
  他プロセスが最終パスを作ると `MoveFileExW` が既存上書き不可で稀に失敗する（データ損失はなし）。
- 修正: `MoveFileExW` のフラグに `MOVEFILE_REPLACE_EXISTING` を追加（`MOVEFILE_WRITE_THROUGH` と併用）。
  窓内で既存が現れても改名がアトミックに成功し、偽陰性での保存失敗を避ける。

## 変更ファイル一覧

- `src/util/encoding.cpp` — `wide_to_cp932` に `WC_NO_BEST_FIT_CHARS` 付与（HIGH-1）。意図を制約
  コメントで明記。
- `src/util/encoding.h` — 往復不変条件の方針を冒頭コメントに明文化（MEDIUM）。挙動変更なし。
- `src/util/atomic_file.cpp` — `FlushFileBuffers` 戻り値検査＋失敗時 tmp 削除＆Io エラー（HIGH-2）、
  `MoveFileExW` に `MOVEFILE_REPLACE_EXISTING` 追加（LOW）。
- `tests/util/encoding_test.cpp` — ベストフィット境界文字の保存中断を観測する
  `ShiftJisRejectsBestFitBoundaryChars`（U+301C・U+2212・U+1F600）と、真の CP932 マッピング文字が
  表現可能かつ往復不変であることを観測する `ShiftJisAcceptsTrueCp932Symbols`（U+3000・U+30FC・
  U+FF5E・U+FF01）を追加。

## must / should criteria 実装状況（sprint 2）

### must
- EncodingDetector（BOM 優先 → BOMなし UTF-8→Shift_JIS、× LF/CRLF 往復維持）:
  充足（既存 `EncodingDetect*`・`EncodingRoundTrip`・`EncodingRoundTrip2` で観測。本ターン無変更）。
- 書き出し時の表現可能性チェック（Shift_JIS 表現不能文字で保存中断）: **本ターンで構造的抜け穴を是正し充足**。
  `WC_NO_BEST_FIT_CHARS` により真に未マップな文字（U+301C/U+2212/絵文字）で `can_encode=false`・
  `encode` が `ErrorCode::Encoding` を返して中断することを `ShiftJisRejectsBestFitBoundaryChars` /
  `ShiftJisRejectsEmoji` で観測。真の CP932 文字は過剰拒否しないことを `ShiftJisAcceptsTrueCp932Symbols`
  で観測。
- ハッシュ（LF 正規化後 XXH3 が CRLF/LF のみ差で一致）: 充足（既存 `hash_test.cpp`。本ターン無変更）。
- AtomicFile（temp→rename・書き込み途中で元ファイル無傷）: 充足。本ターンで `FlushFileBuffers` 失敗時の
  非昇格（HIGH-2）と TOCTOU 緩和（LOW）を加え「データ確定の保証なしで最終パスへ昇格しない」を強化。
  既存 `atomic_file_test.cpp` 7 件 PASS。
- コア公開 API が Result<T>（例外を投げずエラー値）: 充足（既存 `result_test.cpp`・各モジュールが Result。
  本ターンの修正も例外を投げず Io/Encoding エラー値を返す）。

### should
- TaskRunner（2〜4 ワーカー・投入/完了取得）: 既存実装＋`task_runner_test.cpp` で充足（本ターン無変更）。
- Logger（内容を書かずパス/操作/エラーのみ）: 既存実装＋`logger_test.cpp` で充足（本ターン無変更）。

## テスト化できなかった criteria とその理由

- なし（sprint 2 の must/should はすべて gtest で観測している）。本ターンの修正点（HIGH-2 の
  FlushFileBuffers 失敗・LOW の TOCTOU レース）は実 FS で失敗注入が困難なため、専用の失敗注入テストは
  追加していないが、既存 `FailedWriteDoesNotCorruptExisting`（失敗時に元ファイル無傷）と
  `NoTempFileLeftBehindOnSuccess`（成功時 tmp が残らない）が「失敗経路では最終パスを変更しない／tmp を
  残さない」不変条件を観測しており、修正はこの不変条件を成功側に倒さないよう強める方向の変更である。

## 自己実行した verify の結果

sprint 2 の verify は `ctest --preset x64-core-test`。

- `cmake --build --preset x64-core-test`: 成功。`/W4`・警告エラー扱いの設定下で警告ゼロ・エラーゼロ
  （grep で warning/error 検出なし）。
- `ctest --preset x64-core-test`: **100% tests passed, 0 failed out of 2**（`pika_tests_build` PASS /
  `pika_tests` PASS）。gtest 全体は 55 tests / 13 suites がすべて PASS（`EncodingRepresentable` は
  本ターン追加分含め 5 件 PASS、`AtomicFileTest` 7 件 PASS）。

注: 合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

DONE: C:/dev/pika_editor/dev/turns/turn-2-2-generator.md
