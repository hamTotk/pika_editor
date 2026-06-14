# turn-9-1 generator report — sprint 9: core/search（PCRE2 検索・置換エンジン）

## sprint goal（再注入）

core/search(PCRE2)の検索・置換エンジンが実装され、後方参照・キャプチャ参照・Unicode文字クラス・
正規表現置換・巨大入力のキャンセル可能性が gtest で検証される。

中心体験（spec.md）における「見る・確認する・少し直す」の「少し直す」を支える周辺機能。
design.md 3章 core/search「PCRE2、pcre2-16/UTF対応。後方参照・キャプチャ参照・Unicode文字クラス。
巨大/長行入力はワーカーで実行しキャンセル可」を UI/WebView2/Win32 非依存の純ロジックとして実体化した。

## 変更ファイル一覧

新規（src/core/search/）:
- `src/core/search/cancel_token.h` — 協調キャンセル用アトミックフラグ（core/diff と同型・別 namespace）
- `src/core/search/search_types.h` — `SearchOptions`/`Match`/`SearchResult`/`ReplaceResult`/`SearchLimits`（PCRE2 非依存の純データ型）
- `src/core/search/search_engine.h` — `SearchEngine`（公開 API は std::string=UTF-8 と Result<T> のみ）
- `src/core/search/search_engine.cpp` — PCRE2(pcre2-16/UTF+UCP) 実装。UTF-8↔UTF-16 変換＋バイトオフセット写し戻し・キャプチャ参照展開

新規（tests/）:
- `tests/core/search/search_engine_test.cpp` — gtest 26 ケース

既存編集（ビルド配線のみ。テストの緩和・無効化なし）:
- `src/CMakeLists.txt` — search ソースを pika_core に追加、`find_package(pcre2 CONFIG REQUIRED)` と `PCRE2::16BIT` を PRIVATE リンク（公開ヘッダに PCRE2 型を出さない）、スプリント進捗コメント更新
- `tests/CMakeLists.txt` — `core/search/search_engine_test.cpp` を pika_tests に追加

注: `vcpkg.json` には既に pcre2 が依存として宣言済み（変更なし）。

## must criteria の実装状況（すべて自動テスト化済み）

| must criteria | 実装 | テスト |
|---|---|---|
| 検索: インクリメンタル・大文字小文字区別・単語単位・正規表現・全ヒット列挙とヒット件数を返す | `find_all` + `SearchOptions`(case_sensitive/whole_word/regex) + `SearchResult.count()` | `LiteralFindsAllHitsAndCount`/`CaseSensitiveDistinguishesCase`/`CaseInsensitiveMatchesAllCases`/`WholeWordDoesNotMatchSubstring`/`RegexMatchesPattern`/`LiteralTreatsMetacharsLiterally`/`CapturesGroupsInMatch` |
| 正規表現置換: キャプチャ参照(後方参照)を用いた全置換が正しい結果を返す | `replace_all` + `$1`/`${12}`/`\1`/`$$`/`$0` 展開 | `RegexReplaceWithCaptureReferenceDollar`/`RegexReplaceWithBackslashReference`/`RegexReplaceBracedGroupAndDollarLiteral`/`LiteralReplaceDoesNotExpandReferences`/`ReplaceAllPreservesUnmatchedText`/`ReplaceNoMatchReturnsOriginal` |
| Unicode文字クラス: PCRE2(pcre2-16/UTF)で Unicode 文字クラスを含むパターンがマッチ | `PCRE2_UTF\|PCRE2_UCP` でコンパイル。UTF-8→UTF-16→照合→UTF-8 バイト位置写し戻し | `UnicodePropertyMatchesNonAscii`(\p{L})/`UnicodeWordClassIncludesNonAscii`(\w が日本語含む)/`NonAsciiByteOffsetsAreUtf8`/`SurrogatePairOffsetsMapToUtf8`(BMP外=絵文字) |
| 巨大/長行入力の検索・全置換がキャンセル可能、協調キャンセルで中断できる | `CancelTokenPtr` をヒット反復ループ先頭で観測（別スレッド中断に頼らない）。`SearchLimits` で開始前サイズガード | `PreCancelledFindReturnsCancelled`/`PreCancelledReplaceReturnsCancelled`/`CancelDuringIterationStopsAndFlags`/`NotCancelledWhenTokenClear`/`OversizeInputTruncatesBeforeSearching`/`MaxMatchesCapTruncates` |

補助（Result<T> 方式・例外を投げない）: 不正な正規表現は `Result::err(InvalidArgument)` を返す
（`InvalidRegexReturnsErrorNotException`）。CLAUDE.md「コア公開 API は Result<T>。例外はモジュール
内部に閉じる」に従い、PCRE2 のエラーは境界でエラー値化している。

## should criteria の実装状況

| should | 状況 |
|---|---|
| 第2段階(200MB超相当)の読み取り専用では置換が無効化される判定がある | 充足。`SearchLimits.max_total_bytes` 既定 200MB を超える入力では `replace_all` が `truncated=true` を返し置換を開始しない（= 200MB 超の置換無効化の決定論判定）。テスト `OversizeInputTruncatesReplace`（および `OversizeInputTruncatesBeforeSearching` が検索側）で観測。requirements.md 5.4「第2段階(200MB超)の読み取り専用モードでは置換を無効化する」と整合。UI 側の段階フラグ参照（design.md G4・DocState）は core/document/UI 層の責務のため本スプリント対象外 |

## テスト化できなかった criteria とその理由

なし。must 4 件・should 1 件はすべて gtest で観測可能な挙動として検証している。

補足（テスト化しない設計判断）: requirements.md 5.4 の「プレビュー内検索は WebView2 の検索機能で
提供」「検索 UI のモード切替」「進捗表示」は UI（WebView2/wxWidgets）層の責務であり、core/search の
範囲外。本スプリントは spec.md 補完判断 2/3・design.md 13章「自動単体テストの対象は core/・util」に
従い、PCRE2 を内部実装に閉じた純ロジック（SearchEngine）の決定論検証に限定した。

## 設計上の判断（ドリフト追跡用）

- **pcre2-16/UTF を採用**（design.md 3章明記）。pika 文字列は UTF-8(std::string) 統一のため、検索時に
  UTF-8→UTF-16 変換しつつ「各 UTF-16 コードユニット→元 UTF-8 バイト位置」の対応表を構築し、PCRE2 の
  照合結果（UTF-16 単位オフセット）を UTF-8 バイト位置へ写し戻す。サロゲートペア（BMP外）両ユニットは
  同一 UTF-8 先頭バイトに対応付ける。これで公開 API のオフセットを UTF-8 バイト基準に統一できる。
- **PCRE2 を公開ヘッダに出さない**: `PCRE2_CODE_UNIT_WIDTH`/pcre2.h は search_engine.cpp のみ、リンクは
  `PCRE2::16BIT` を PRIVATE。CLAUDE.md「コアはUIを知らない」「公開ヘッダに外部型を出さない」整合
  （toml11/md4c/dtl と同じ扱い）。
- **協調キャンセル**: PCRE2 の 1 マッチ呼び出しは内部キャンセル点を持たないため、ヒット反復という
  pika 側で刻める区切りでアトミックフラグを観測する（design.md 8章 I6「別スレッド中断に頼らない」と
  同思想。core/diff CancelToken と同型）。CancelToken は core/diff と別 namespace に独立配置（コア
  サービス同士は原則独立。モジュール間依存を増やさない）。
- **開始前サイズガード**: `SearchLimits`（max_total_bytes 既定 200MB / max_matches 既定 100万件）で
  入力過大を開始前に判定し `truncated` を返す（DiffLimits と同じ思想）。

## 自己実行した verify の結果

verify コマンド（sprints.json sprint 9）:
- `cmake --preset x64-core-test` → CONFIGURE_EXIT=0（vcpkg manifest 解決＝pcre2 含む・CMake 構成成立）
- `ctest --preset x64-core-test` → CTEST_EXIT=0（pika_tests_build / pika_tests とも Passed。100% tests passed, 0 failed）

補助確認:
- `./build/x64-core-test/tests/Release/pika_tests.exe --gtest_filter='SearchEngineTest.*'` →
  26 ケース全 PASS（[ PASSED ] 26 tests.）
- `cmake --build ... --target pika_core`（/W4 /WX）→ warning/error なし（PCRE2 外部ヘッダは
  pika_warnings 非適用で警告波及せず）
- clang-format（編集直後 hook）→ 全ファイル CLEAN

合否判定の正本は run-dev が別途実行する結果。上記は自己確認であり判定には用いない。

DONE: C:/dev/pika_editor/dev/turns/turn-9-1-generator.md
