# turn-7-1 generator report — sprint 7（core/ipc: CLI とシングルインスタンス）

## sprint goal（再注入）

CLI とシングルインスタンスの引数解釈・転送ロジック（pika.com スタブ含む）を実装し、
-g パース・引数検証・終了コード・IPC スキーマ検証を gtest で検証する。エージェント向け API の
土台を貫通させる。

UI/Win32 非依存のコアロジックに切り出し、`x64-core-test` プリセットで決定論検証できる形にした
（spec.md 補完判断3「watcher/サニタイズ等は wx 非依存コアに切り出して gtest 検証」と同方針）。
実際の名前付きパイプ I/O・ウィンドウ生成はプラットフォーム層（GUI スプリント）の責務とし、本
スプリントは「方針・判定・スキーマ」を決定論ロジックとして実装した。

## 変更ファイル一覧

新規（src/core/ipc/）:
- `src/core/ipc/cli_parser.h` / `cli_parser.cpp` — `-g` パース・argv 解釈・引数検証/分類・終了コード
- `src/core/ipc/ipc_message.h` / `ipc_message.cpp` — IPC 転送 JSON のシリアライズ/スキーマ検証
- `src/core/ipc/path_normalizer.h` / `path_normalizer.cpp` — 相対パスの CWD 基準絶対パス正規化
- `src/core/ipc/pipe_security.h` / `pipe_security.cpp` — パイプ命名・DACL(SDDL)・保護方針

新規（tests/core/ipc/）:
- `tests/core/ipc/cli_parser_test.cpp`（24 ケース）
- `tests/core/ipc/ipc_message_test.cpp`（13 ケース）
- `tests/core/ipc/path_normalizer_test.cpp`（9 ケース）
- `tests/core/ipc/pipe_security_test.cpp`（3 ケース）

変更:
- `src/CMakeLists.txt` — pika_core に core/ipc の 8 ファイルを追加、sprint 7 コメント追記
- `tests/CMakeLists.txt` — pika_tests に core/ipc の 4 テストを追加
- `src/app/main_console.cpp`（pika.com）— sprint 1 のスタブから、core/ipc を使う実体に更新
  （`--help`/`--version` 出力・引数検証の同期実行・終了コード分類を橋渡し）

## must criteria 実装状況（すべて gtest 化済み）

1. **-g パース**（`<file>:<行>[:<桁>]` 末尾剥がし・ドライブレターのコロン非分割・桁省略=行頭・
   非整数は位置指定無視）→ `parse_goto`（cli_parser.cpp）。
   検証: `CliGoto.*`（8 ケース。`C:\dir\doc.md:42` がドライブのコロンを割らず 42 行を剥がす、
   `doc.md:abc` は位置指定なし、桁省略で column=1 等）。

2. **引数検証**（存在しないフォルダ=非0、存在しないファイルパス=新規タブ受理の分類）→
   `validate`（PathProbe 注入で FS 非依存）。
   検証: `CliValidate.MissingFolderTrailingSeparatorIsError`（非0）、
   `CliValidate.MissingFileAcceptedAsNewTab`（受理・NewFile）、
   `CliValidate.MultipleFoldersIsError`（単一フォルダ方針）等。

3. **相対パス正規化**（クライアント側 CWD 基準で絶対パス化してから転送）→ `normalize_to_absolute`
   （CWD を引数注入・FS 非アクセス・`.`/`..` 解決・区切り統一）。
   検証: `PathNormalize.*`（9 ケース。相対連結・`..` のルート止め・ドライブ相対の同/別ドライブ・
   UNC 保持・実在しないパスも正規化）。

4. **IPC スキーマ検証**（1行・最大数KB・スキーマ不一致は破棄・受理操作をパスのオープンに限定）→
   `parse_request`（ipc_message.cpp。json_lite を再利用しネスト深さ上限・破損入力 false を継承）。
   検証: `IpcMessage.*` / `IpcAbsPath.*`（13 ケース。op!=open 破棄・相対パス破棄・非整数 line 破棄・
   targets 非配列破棄・kMaxMessageBytes 超過破棄・往復一致・空 targets 受理）。

5. **終了コード規約**（受理=0・不正引数=非0 の分類）→ `enum class ExitCode`（Accepted=0 /
   InvalidArgument=2）と `ValidationResult.exit_code`。
   検証: `CliValidate.ExitCodeContract`（0 と非0 を明示観測）、各 validate ケースが exit_code を確認。

## should criteria 実装状況

- **pika.com スタブの --help/--version がパイプ・リダイレクトで欠落/文字化けしない**:
  `main_console.cpp` を core/ipc 経由に更新し、`printf`/`puts`（行バッファ・UTF-8。ルート
  CMake が `/utf-8`）で出力。help/version は GUI を起動せず 0 で即終了。
  出力内容の自動 gtest 化は console exe の I/O のため見送り（下記「テスト化できなかった criteria」）。
  解釈ロジック（`parse_argv` が --help/-h/--version を最優先で拾う）は `CliParseArgv.HelpAndVersion`
  で gtest 化済み。

- **名前付きパイプ `\\.\pipe\pika-<SID>` の DACL(作成者のみ)＋PIPE_REJECT_REMOTE_CLIENTS の方針**:
  `pipe_security`（`make_pipe_name`・`make_owner_only_sddl`・`default_policy`）で命名・SDDL・
  フラグ方針を組み立て。検証: `PipeSecurity.*`（パイプ名が SID を含む、SDDL が当該 SID のみ GA を
  Allow し Everyone(WD)/Anonymous(AN) を含まない、既定方針が reject_remote_clients=true）。

## テスト化できなかった criteria とその理由

- **console exe の標準出力バイト列（--help/--version のパイプ/リダイレクト欠落・文字化けの実機確認）**:
  プロセス起動＋パイプキャプチャを要し、`pika_tests`（pika_core にリンクする gtest）の決定論
  ユニット範囲を外れる。出力を生む解釈ロジック（parse_argv の help/version 判定）は gtest 化済み。
  実バイト列の確認は `docs/acceptance.md` の手動チェックリスト側（spec.md 検証手段）に委ねる。

- **名前付きパイプの実 I/O・TryAcquire の起動レース・サーバー/クライアント転送**:
  Win32 `CreateNamedPipe`/DACL 適用は OS 副作用を伴いプラットフォーム層の責務（design.md 5.1）。
  本スプリントでは「どの名前・どの SDDL・どのフラグで保護するか」の方針を決定論ロジックとして
  切り出し gtest 化した。Win32 適用自体は GUI 統合スプリントで配線する。

## 自己実行した verify 結果（合否の正本は run-dev）

- `cmake --preset x64-core-test` → exit 0（vcpkg manifest 解決＋構成成立）
- `ctest --preset x64-core-test` → exit 0（`pika_tests_build` Passed / `pika_tests` Passed、
  100% tests passed, 0 failed）
- 新規 IPC スイートのみ抽出実行 → `47 tests from 7 test suites` 全 PASSED
- `clang-format --dry-run --Werror`（全新規/変更ファイル）→ 差分なし
- /W4・/WX 下でビルド成立（警告ゼロ。警告があれば pika_tests_build が失敗する構成）

## 設計遵守メモ

- レイヤー: core/ipc は util のみ依存（ipc_message は core/snapshot/json_lite を内部再利用するが
  これは UI 非依存コア同士で逆参照ではない）。wx/Win32/WebView2 を一切 include しない。
- Result<T>: CLI 検証は `ValidationResult`（accepted/exit_code/message）で例外を投げず分類を返す。
  IPC スキーマは破損入力で false を返し例外を投げない（信頼境界の堅牢化）。
- YAGNI: パイプ実 I/O・ジャンプリスト・エクスプローラー統合（要件3.3）は本スプリント goal 外のため
  実装せず、方針層のみに留めた。

DONE: C:/dev/pika_editor/dev/turns/turn-7-1-generator.md
