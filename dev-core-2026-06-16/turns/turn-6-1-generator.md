# turn 6-1 generator report — sprint 6 (core/diff)

## sprint goal（再注入）

core/diff の行差分（dtl/Myers）と行内の単語/文字単位ハイライト、累積差分の基盤を実装し、
LF正規化照合・大規模入力ガード・色非依存（+/-）出力を gtest で検証する。中心体験
（spec.md「中心体験」4「前回確認時点からの累積差分を赤/緑で確認」）の決定論部分を固める。

## 変更ファイル一覧

新規（src/core/diff/）:
- `src/core/diff/diff_types.h` — `LineOp`/`DiffLine`(色非依存 `marker()`)/`InlineSpan`/`DiffResult`/`DiffLimits`
- `src/core/diff/cancel_token.h` — `CancelToken`（協調キャンセルのアトミックフラグ）/`CancelTokenPtr`
- `src/core/diff/line_diff.h` / `line_diff.cpp` — `split_lines_lf`（LF正規化行分割）/`diff_lines`（dtl Myers・unified順）
- `src/core/diff/inline_diff.h` / `inline_diff.cpp` — `compute_inline_spans`（語LCS→日本語等は文字単位LCSフォールバック）
- `src/core/diff/diff_engine.h` / `diff_engine.cpp` — `DiffEngine::compute`（累積差分・開始前サイズガード・行内強調付与・キャンセル）

新規テスト（tests/core/diff/）:
- `tests/core/diff/line_diff_test.cpp`（9 ケース）
- `tests/core/diff/inline_diff_test.cpp`（7 ケース）
- `tests/core/diff/diff_engine_test.cpp`（9 ケース）

ビルド配線:
- `src/CMakeLists.txt` — core/diff の .cpp/.h を pika_core に追加。dtl はヘッダオンリーで CMake config を
  持たないため `find_path(DTL_INCLUDE_DIR NAMES dtl/dtl.hpp REQUIRED)` で解決し、`line_diff.cpp` の
  内部実装だけで使うため pika_core に PRIVATE include（公開ヘッダに dtl 型を漏らさない）
- `tests/CMakeLists.txt` — 3 テスト .cpp を pika_tests に追加

## must criteria 実装状況（すべて自動テスト化）

| # | criterion | 実装/テスト | 状況 |
|---|-----------|------------|------|
| 1 | 行差分: LF正規化後に行分割して Myers 差分→追加/削除/変更行 | `diff_lines`（dtl）/ `LineDiffTest.ReportsAddDeleteChange`・`AllAddedWhenBaselineEmpty`・`AllDeletedWhenCurrentEmpty`・`DiffEngineTest.CumulativeDiffCountsAddedAndRemoved` | 充足 |
| 2 | 改行のみの差: CRLF/LF のみ異なる同一内容で差分が空 | `split_lines_lf` が CRLF→LF 正規化 / `LineDiffTest.CrlfVsLfOnlyDiffIsEmpty`・`DiffEngineTest.CrlfVsLfOnlyDiffIsEmpty` | 充足 |
| 3 | 行内ハイライト: 空白区切りトークンの LCS、日本語等は文字単位 LCS フォールバック | `compute_inline_spans`（`words_are_separable` で分岐）/ `InlineDiffTest.WordLevelHighlightsChangedWord`・`JapaneseFallsBackToCharLevel`・`CharFallbackDoesNotSplitMultibyte`・`MixedAsciiAndJapaneseUsesCharFallbackWhenNoSpaces` | 充足 |
| 4 | 色非依存出力: 各差分行に追加/削除を示す +/- 記号クラスが必ず付く | `DiffLine::marker()` が `+`/`-`/` ` / `LineDiffTest.MarkerIsColorIndependent`・`DiffEngineTest.EveryLineHasColorIndependentMarker` | 充足 |
| 5 | 大規模入力ガード: 行数・最長行長が上限超過（10MB相当）なら計算を開始せずフォールバック（別スレッド中断に頼らない） | `DiffEngine::compute` が `exceeds_limits` で開始前判定→`truncated=true`・lines 空 / `DiffEngineTest.LargeByteInputTruncatesBeforeComputing`・`TooManyLinesTruncates`・`OverlongLineTruncates` | 充足 |

## should criteria 実装状況

| criterion | 実装/テスト | 状況 |
|-----------|------------|------|
| 空ファイル・全行追加（ベースラインなし）・全行削除の境界 | `LineDiffTest.AllAddedWhenBaselineEmpty`・`AllDeletedWhenCurrentEmpty`・`IdenticalContentHasNoChanges`・`DiffEngineTest.EmptyVsEmptyHasNoChanges`・`SplitLinesLfTest.*` | 充足 |
| 差分計算がキャンセル可能（協調キャンセルのアトミックフラグ） | `CancelToken`（`std::atomic<bool>`）を `DiffEngine::compute` の各段で観測 / `DiffEngineTest.PreCancelledReturnsCancelled`・`NotCancelledWhenTokenClear` | 充足 |

## テスト化できなかった criteria とその理由

なし。sprint6 の must 5 件・should 2 件はいずれも wx/WebView2 非依存の純ロジックであり、
すべて gtest で観測可能な挙動として検証した（テスト化不能項目はない）。

補足（本層のスコープ境界・後続申し送り。テスト化対象外＝criteria に含まれない）:
- 差分の **HTML 出力**（unified HTML・テーマCSS）は design.md 8章で `PreviewBuilder`（render/UI 層）の
  責務であり本スプリント（core/diff の計算）対象外。本層は `DiffResult`（行・op・記号・強調区間）まで
  を返し、HTML 化は後続の render/UI 結線で行う。色非依存の記号は `DiffLine::marker()` で API として提供済み。
- 「10MB以上自動オフ」の閾値の **実値結線**（settings からの上書き）は sprint8 settings/結合スプリント
  側。本層は `DiffLimits` 既定値（10MB/200000行/100000バイト・設定可能）で開始前ガードを実装済み。
- ベースライン内容の **供給元**（snapshot object・10MB以上/画像はハッシュのみで差分非活性）は
  sprint5 snapshot と sprint10 結合の責務。本層は内容（string）を受け取れば計算する純関数で、
  非活性判定の結線は後続（design.md 8章 D2）。

## 設計上の判断メモ

- **unified 順の保証**: dtl の SES は変更ブロック内で add/delete を任意順で出す（実測で ADD が
  DELETE より先に出るケースを確認）。そのため `diff_lines` はブロック内の削除・追加を両方バッファし、
  COMMON（または末尾）で「削除群→追加群」に確定する（`flush_block`）。これにより dtl の内部順に
  依存せず unified 表示（要件8章）になる。`LineDiffTest.UnifiedOrderPlacesDeletesBeforeAdds` で観測。
- **行内強調のペアリング**: `DiffEngine::attach_inline_spans` が連続 Delete 群とその直後の Add 群を
  位置対応（Delete[k]↔Add[k]）させて `compute_inline_spans` を適用する。行数が増減した余剰側は
  全体が新規/消滅で部分差がないため強調を付けない。
- **文字単位フォールバックの境界安全**: `tokenize_chars` は UTF-8 先頭バイトからコードポイント長を
  求めて分割し、強調区間が文字を割らない。`InlineDiffTest.CharFallbackDoesNotSplitMultibyte` で
  区間端が継続バイト（0x80-0xBF）で始まらないことを観測。
- **ガードは開始前判定**: dtl/inline LCS は途中中断不可のため、入力サイズ（バイト・行数・最長行長）で
  計算開始前に `truncated` 判定する（design.md 8章 I6「タイムアウトを別スレッド中断に頼らない」）。

## 自己実行した verify の結果

合否判定の正本は run-dev が別途実行する結果。以下は自己確認。

- `cmake --preset x64-core-test` → exit 0（vcpkg manifest 解決・dtl `find_path` 解決・CMake 構成成立）
- `ctest --preset x64-core-test` → 100% tests passed, 0 failed（pika_tests_build / pika_tests の 2 テスト）
  - 全体: 242 tests, 240→（修正後）全 PASS
  - sprint6 新規分: `LineDiffTest.* / SplitLinesLfTest.* / InlineDiffTest.* / DiffEngineTest.*` の
    25 ケースすべて PASS

初回 ctest で 2 件失敗（`LineDiffTest.UnifiedOrderPlacesDeletesBeforeAdds`・
`DiffEngineTest.ChangedLinePairGetsInlineSpans`）→ dtl の add/delete 出力順の前提誤りが原因と特定し、
`diff_lines` のブロックバッファ方式（削除群→追加群を一括確定）へ修正して解消。テストは緩和・スキップ
していない（criteria の観測対象＝unified 順・行内強調付与を維持したまま実装側を直した）。

DONE: C:/dev/pika_editor/dev/turns/turn-6-1-generator.md
