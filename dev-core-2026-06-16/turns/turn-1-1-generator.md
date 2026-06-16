# turn-1-1 generator report — sprint 1（ビルド基盤）

- sprint_id: 1 / iteration: 1 / feedback: null（初回）
- goal: ビルド基盤が成立し、wx 非依存の `pika_core` + util と gtest(`pika_tests`) が
  `x64-core-test` プリセットで構成・ビルド・テストできる。重量依存（wx/WebView2）を含まない
  最小構成で ctest が exit 0 を返す。

## 変更ファイル一覧（すべて project_dir 内）

新規作成（git 未追跡）:

- `C:/dev/pika_editor/CMakeLists.txt` — ルート。ターゲット分割・/W4/WX・/MT・/O2・LTCG 方針
- `C:/dev/pika_editor/CMakePresets.json` — `x64-core-test` / `x64-release` / `x64-debug` プリセット
- `C:/dev/pika_editor/vcpkg.json` — manifest（core-test 依存を default、wx は `gui` feature に隔離）
- `C:/dev/pika_editor/vcpkg-configuration.json` — builtin-baseline を SHA ピン留め（再現ビルド）
- `C:/dev/pika_editor/src/CMakeLists.txt` — `pika_core`（静的lib, core/+util）の定義
- `C:/dev/pika_editor/src/util/hash.h` / `hash.cpp` — util の XXH3 ハッシュ（最小実体）
- `C:/dev/pika_editor/src/app/CMakeLists.txt` — `pika`（GUI exe）/ `pika.com` スタブ（PIKA_BUILD_GUI 限定）
- `C:/dev/pika_editor/src/app/main_gui.cpp` — wx 最小 App スタブ
- `C:/dev/pika_editor/src/app/main_console.cpp` — pika.com の --help/--version 骨格
- `C:/dev/pika_editor/tests/CMakeLists.txt` — `pika_tests`（gtest）。ctest 自己ビルドフィクスチャ付き
- `C:/dev/pika_editor/tests/util/hash_test.cpp` — XXH3 スモークテスト（5 ケース）

既存ファイルは Read のみで一切書き換えていない（`.clang-format`/`.github/`/`docs/`/`dev/spec.md`/
`dev/sprints.json` 含む。CI yml は deny ルール対象かつ sprint 1 で改変不要のため触れていない）。

## must criteria の実装状況（すべて充足）

1. **ルート CMakeLists.txt と vcpkg.json・vcpkg-configuration.json が存在し、x64-core-test プリセット
   （core/+util と gtest のみ、wx/WebView2 を含まない）が定義されている** → 充足。
   `x64-core-test` は `VCPKG_MANIFEST_FEATURES=tests`（gtest のみ）を有効化し、wxwidgets は
   `gui` feature に隔離して取得しない。インストール結果に wx/webview ヘッダが無いことを確認
   （`vcpkg_installed/x64-windows-static/include` に wx/webview なし）。

2. **cmake --preset x64-core-test が exit 0 で構成成立する（vcpkg manifest 解決を含む）** → 充足。
   初回は vcpkg が md4c/dtl/pcre2/zstd/xxhash/toml11/gtest を 57s で install、CMake 構成 done。
   exit 0 を確認。再構成は約 4.6s（vcpkg バイナリキャッシュ）。

3. **pika_core（静的lib, core/+util）と pika_tests（gtest）がビルドでき、最小スモークテストが
   ctest --preset x64-core-test で 1 件以上 PASS する** → 充足。
   `pika_core.lib` 生成、`pika_tests.exe` 生成。XXH3 スモーク 5 ケースが全 PASS（ctest exit 0）。

4. **/W4・警告エラー扱いの設定下で core/util のコードが警告ゼロでビルドされる** → 充足。
   `pika_warnings`(INTERFACE) で `/W4 /WX /permissive-` を自前ターゲットにのみ適用。生成 vcxproj に
   `WarningLevel=Level4` / `TreatWarningAsError=true` / `ExternalWarningLevel=TurnOffAllWarnings`
   （`/external:I` で vcpkg include を警告除外 = design.md 12章「外部ヘッダ除く」）を確認。
   ビルドログに自前コードの警告ゼロ。

補足（must を超えるが goal の根拠として確認したビルド方針）:
`RuntimeLibrary=MultiThreaded`(/MT)、`Optimization=MaxSpeed`(/O2)、`WholeProgramOptimization=true`
(LTCG) が Release 構成で有効。triplet は `x64-windows-static`（依存も静的リンク）。

## should criteria の実装状況

1. **x64-release プリセット（wx を含む pika GUI exe）の CMake 構成定義が存在する** → 充足（定義のみ）。
   `cmake --list-presets` に `x64-release` が列挙される。spec 補完判断 2 に従い、初回重量ビルドの
   完走は verify 必須にしていない（実行未確認）。

2. **ターゲット分割が design.md 12章どおり pika_core / pika / pika.com / pika_tests** → 充足。
   `pika_core`（静的lib）/ `pika`(WIN32 exe) / `pika.com`（`pika_com` ターゲットを OUTPUT_NAME=pika・
   SUFFIX=.com で出力）/ `pika_tests` を定義。pika/pika.com は `PIKA_BUILD_GUI` で core-test 時は不構成。

3. **.github/workflows/ci.yml の build-test ジョブが将来 if:false を外せる前提と整合** → 充足。
   ci.yml は `x64-release` プリセット・`cmake --build --preset` / `ctest --preset` を前提にしており、
   本実装のプリセット名・ターゲット分割と一致する（ci.yml は未編集）。

## テスト化できなかった criteria とその理由

- must は全件、自動テストまたは「verify コマンドの exit 0」で観測可能化できた（テスト化漏れなし）。
- should 1（x64-release 完走）と should 3（CI if:false 解除）は spec 補完判断 2 のとおり初回重量ビルド
  timeout リスクのため自動 verify に含めず、構成定義の存在（`--list-presets`）と整合のみで確認した。
  これは sprints.json の should の「初回重量ビルドの完走は必須にしない」と一致する。

## verify 自己実行の結果

新規チェックアウト相当（build ディレクトリ削除後）で 2 段を実行:

```
cmake --preset x64-core-test   -> exit 0（vcpkg 解決 + 構成 done）
ctest --preset x64-core-test   -> exit 0（2/2 PASS。内訳: pika_tests_build フィクスチャ + pika_tests）
```

`pika_tests`（1 ctest テスト）の内部で gtest 5 ケース全 PASS（SameInputSameHash /
DifferentInputDifferentHash / EmptyInputIsStable / HexIs16LowerHexChars / HexMatchesSnprintf）。

### 設計判断（評価者向けの根拠）

- spec.md「自動検証」の verify は `cmake --preset`（構成のみ）→ `ctest --preset` の 2 段で、明示の
  `cmake --build` 段が無い。新規チェックアウトでは構成のみだとテスト実体が未ビルドで
  `gtest_discover_tests` が NOT_BUILT プレースホルダで失敗する（検証済み）。そこで
  `tests/CMakeLists.txt` に **ctest 実行時にテストをビルドする setup フィクスチャ**
  （`pika_tests_build`）を登録し、テスト本体を `FIXTURES_REQUIRED` で依存させた。これにより
  `ctest --preset` 単体で「ビルド→実行」が完結し、フレッシュ/既ビルド双方で決定論的に exit 0 になる。
  gtest_discover_tests は未ビルド時に失敗するため使わず、テスト実行体を 1 つの ctest テストとして
  登録（gtest_main が内部で全ケースを走らせる）。テストの緩和・スキップ・無効化は一切していない。
- vcpkg baseline は手元 vcpkg(`C:\dev\vcpkg`)の HEAD `44819aa…` にピン留め（再現ビルドのため
  vcpkg.json の builtin-baseline と vcpkg-configuration.json を一致させた）。
- 生成コードは sprint 1 goal（ビルド基盤の成立）に必要な最小実体のみ。util は XXH3 のスモーク対象に
  限定し、LF 正規化ハッシュ・エンコーディング・watcher 等は後続スプリントへ（YAGNI / spec の
  スプリント割当に従う）。

DONE: C:/dev/pika_editor/dev/turns/turn-1-1-generator.md
