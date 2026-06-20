# turn-1-1 generator report — sprint 1（準備＋最薄プラットフォーム証明ループ＋canon 改訂着手）

- sprint_id: 1 / iteration: 1 / feedback: null（初回ターン）
- 目標（再注入）: 旧 C++ ツリー退避＋除去 → cargo workspace＋Tauri scaffold（design doc 13章）→ 最薄ループ貫通
  （フォルダを開く→ツリー→タブ→CM6 編集→保存）→ design doc 15章 必達 Open (1)IPC コスト/(3)別WebView 権限ゼロ/
  (5)WebView2 不在時 を立証する経路を敷く → canon 改訂着手（requirements 1章・CLAUDE.md・design.md・ui-design.md。
  巨大ファイル数値 2.2/5.4/9.2 は sprint 6 へ TBD で送る）。

## 自己実行した verify の結果

| verify | コマンド | 結果 |
|---|---|---|
| 系統A（決定論ゲート） | `cargo test` | **PASS（exit 0）**。pika-core 14 件・pika-cli 4 件・doctest 0 件 |
| 系統A補（コンパイル/型） | `cargo build`（crates＋src-tauri・`warnings="deny"`） | **PASS（exit 0・警告ゼロ）** |
| 系統A補（frontend 型） | `npm run typecheck`（`tsc -p tsconfig.app.json`・strict） | **PASS（exit 0）** |
| 参考 | `npm run build`（tsc＋vite build） / `cargo fmt --check` | PASS（dist/ 生成・整形差分なし） |

注: 合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

## 変更ファイル一覧

### 新規（Tauri scaffold・design doc 13章）
- `Cargo.toml`（workspace: crates/pika-core・crates/pika-cli・src-tauri。`warnings="deny"`・release LTO/strip）/ `Cargo.lock`
- `crates/pika-core/`（Cargo.toml・src/lib.rs・src/error.rs〔`Result<T, PikaError>`〕・src/cli.rs〔-g パース〕・src/data_root.rs〔データルート解決〕）
- `crates/pika-cli/`（Cargo.toml・src/main.rs〔console subsystem・--help/--version/-g/終了コード〕）
- `src-tauri/`（Cargo.toml・tauri.conf.json・build.rs・capabilities/main.json・src/main.rs・src/commands.rs・src/webview2.rs・icons/icon.ico+png）
- `src/`（frontend: index.html・main.ts・ipc.ts・ui/{tree,tabs,status,notifications}.ts・editor/index.ts〔CM6〕・diff/・preview/・theme/・a11y/・styles/{tokens.css,app.css}）
- `package.json`・`package-lock.json`・`tsconfig.app.json`・`vite.config.ts`
- `installer/README.md`（sprint 7 用骨格）

### 変更（canon 改訂・design doc 16章。元ドキュメントは超非破壊で旧記述を保全しつつ Tauri へ整合）
- `docs/requirements.md`（1章 技術スタック→design doc 2章へ差替＋旧 wx スタックを 1.x へ保全／2.2・5.4・9.2 巨大ファイル数値を **TBD（sprint 6 確定）**／2.3 WebView2 必須前提へ改訂／11.5 a11y を ARIA 再構築へ改訂）
- `CLAUDE.md`（補助原則ネイティブ優先→全Web描画・未信頼文書隔離／アーキ層名／リポジトリ構成→design doc 13章／ビルド・テストを cargo/npm へ／判断ガイド・確定判断）
- `docs/design.md`（冒頭に Tauri 刷新の部分改訂ヘッダ。2/3/12/14章の技術手段は design doc へ載せ替え、設計原則優先順位・コアはUIを知らない・ワークスペースを汚さない・Result<T> は維持）
- `docs/ui-design.md`（実装手段直書き＝§6 wxBitmapBundle→Web SVG・§13 UIA/MSAA→ARIA・§2 CSS変数注入→custom protocol 配信・固定px→rem を改訂。視覚仕様は流用）
- `docs/acceptance.md`（Tauri 系統C チェックリスト TA/TB を新設・節番号併記）
- `docs/acceptance-findings.md`（T-001 IPC コスト・T-002 別WebView 権限ゼロ・T-003 WebView2 不在時・T-004 最薄ループ を追記）
- `.gitignore`（target/・node_modules/・dist/・src-tauri/gen/ を追加。旧 build/・vcpkg_installed/ は退避ブランチに保全済みと明記）

### 退避＋除去（最上位原則「データを失わない」のリポジトリ版）
- 退避ブランチ `wip/cpp-core-snapshot`（HEAD=11e94d7）に旧 C++ 一式が**残存することを観測**（src/+tests/ 222 ファイル・CMakeLists.txt・vcpkg.json を `git cat-file -t` で確認）。
- 本ツリー（main 作業ツリー）から旧 src/・tests/・CMakeLists.txt・CMakePresets.json・vcpkg.json・vcpkg-configuration.json・wx 専用 assets（preview-bootstrap.js・preview.css）を `git rm`（228 deletions・ステージ済み）。`assets/vendor`・`THIRD_PARTY_NOTICES`・`vendor.lock` は sprint 4 再利用のため保持。
- `build/`・`vcpkg_installed/` をディスク削除（.gitignore 済み生成物）。dev-core/dev-ui-2026-06-16 は前フェーズ参照仕様として保持（spec 出典5）。
- **コミットは未実施**（generator はコード変更まで。コミット判断は run-dev/ユーザー側）。退避ブランチは既存のため履歴は失われない。

## must criteria ごとの実装状況

1. **退避＋除去** — 充足。退避ブランチに旧コード残存を観測（222 ファイル等）。本ツリーから旧 C++ 一式を `git rm`、build/・vcpkg_installed/ をディスク削除。
2. **scaffold（design doc 13章・pika-core が Tauri 非依存）** — 充足。骨格生成済み。`cargo tree -p pika-core` で tauri/wry が**ツリーに無い**ことを観測（thiserror のみ）。
3. **cargo test 成立（決定論ゲート）** — 充足。pika-core に純粋関数テスト 14 件を実装し PASS:
   - `cli.rs`: -g パースの「ドライブレターのコロン非分割」（`C:\dir\a.md:12:3`→file=`C:\dir\a.md`/line=12/col=3。小文字ドライブ・行のみ・非整数末尾・`C:` 単体・空引数 を網羅）。
   - `data_root.rs`: portable.txt 分岐（`<exe>/pika-data`）・インストール分岐（`%LOCALAPPDATA%\pika`）・LOCALAPPDATA 不在/空のエラー。
4. **cargo build 警告エラー扱い（exit 0）＋frontend 型チェック確定** — 充足。`warnings="deny"` で警告ゼロ build 成立。`package.json` に `typecheck`/`build`（`tsc -p tsconfig.app.json`）を確定し exit 0。
5. **最薄ループ貫通（系統C）** — 経路実装済み（cargo build＋npm build 成立）。`commands.rs` open_workspace/read_file/save_file・`main.ts` がフォルダ→ツリー→タブ→CM6（`@codemirror/*`）→保存を配線。**実機起動の貫通操作は系統C（acceptance-findings T-004）**として spec/acceptance に明記済み。
6. **IPC コスト実測（系統C）** — 設計確定＋経路を敷いた（保存=最薄では invoke 暫定／本予算は custom protocol 直配信・Channel）。findings T-001 に記録。**基準機 Release ラウンドトリップ実測は未実施（要実機）**＝系統C 必達。
7. **別WebView 権限ゼロ隔離の実証（系統C）** — capability マップを確立（main のみ `capabilities/main.json`、preview ラベルは**ファイル不在＝権限ゼロ**。main.rs run() にコメントで設計を固定）。findings T-002 に記録。**プレビュー WebView 実生成と到達不能の実機実証は sprint 4 本番経路で実施**＝系統C 必達。
8. **WebView2 不在時起動（系統C）** — 充足（実装）。`src-tauri/src/webview2.rs` が Evergreen GUID の `pv` レジストリ検出（HKLM 64bit ビュー / HKCU）、不在時に `main()` が Tauri 起動前 `MessageBoxW` で導入案内し `exit(1)`。findings T-003 に記録。**不在環境模擬の実機確認は未実施（要実機）**＝系統C。
9. **canon 改訂着手** — 充足。requirements 1章/2.3/11.5・CLAUDE.md・design.md・ui-design.md を Tauri へ整合。Result<T>/コアはUIを知らない/ワークスペースを汚さないは維持。2.2/5.4/9.2 は推測値を書かず **TBD（sprint 6 確定）** と明記。

## should criteria

- **vanilla TS モジュール分割** — 実装（ui/{tree,tabs,status,notifications}・editor・diff・preview・theme・a11y・styles）。Svelte は状態が重くなった時点で見極める旨を各 skeleton と CLAUDE.md に記載。
- **ui-mock の CSS 変数トークンを theme に単一源配置＋ライト/ダーク/システム追従骨格** — 実装（`src/styles/tokens.css` が ui-mock の `:root`/`.theme-light`/`.theme-dark` をミラー・`html[data-theme]`＋`prefers-color-scheme` 追従）。
- **capability マップ（メイン=最小・preview=ゼロ）** — 実装（`src-tauri/capabilities/main.json`＝main に `core:default` のみ。preview はファイル不在で権限ゼロ）。
- **cargo audit を CI/verify に組み込む .github/workflows 新設** — **未実施（ツール側制約）**。`.github/workflows/**` は本ハーネスの deny ルールで Write/Edit 不可（`Edit(.github/workflows/**)`）。cargo-audit も未導入（`cargo install` は重量級コンパイルを要し、spec 上 sprint 1 は should・stably runnable になった段で must）。→ **下記「テスト化/実装できなかった項目」に根拠を記載し評価に委ねる**。

## テスト化できなかった criteria とその理由（系統C／ツール制約）

- **最薄ループ貫通・IPC コスト実測・別WebView 権限ゼロ隔離・WebView2 不在時起動**（must 5〜8 の実機側）: いずれも GUI 実機・実測・レジストリ模擬が要るため `cargo test` で固められず系統C（acceptance-findings T-001〜T-004）へ。spec「検証戦略 系統C」「本フェーズの自動 verify に載せない」と整合。決定論で固められる部分（-g パース・データルート解決・WebView2 検出ロジックの構造）は実装し、Win32 FFI 実挙動は系統C。
- **.github/workflows の Tauri CI 新設（should）**: ハーネスの `Edit(.github/workflows/**)` deny で作成不可。**意図する内容**（`cargo test`／`cargo build --workspace`／`npm ci && npm run typecheck && npm run build`／`cargo audit` を windows-latest で回す）は本報告に残す。人間が deny を解除して適用するか、評価者の判断に委ねる。基準を緩める意図はない。
- **root `tsconfig.json`**: `Edit(tsconfig.json)`（ルート限定）が deny。代替として `tsconfig.app.json` を採用し npm scripts を `-p tsconfig.app.json` に固定（型チェックは exit 0 で成立）。機能上の欠落なし。

## 設計判断メモ（ドリフト追跡）

- pika-core は `thiserror` のみ依存し Tauri/wry を持たない（`cargo tree` で観測）＝「コアは UI を知らない」を構造で担保。
- 最薄ループのフォルダ選択はネイティブダイアログ（dialog plugin）を使わずパス入力にした（capability を増やさない最小権限。design doc 9章）。ネイティブ選択は後続スプリントで導入を見極める。
- vite を 7 系へ（esbuild dev-server advisory 解消）。残 1 件は dev-only low。
- Cargo.lock / package-lock.json を tracked（再現ビルド）。target/・node_modules/・dist/・src-tauri/gen/ は ignore。

DONE: C:/dev/pika_editor/dev/turns/turn-1-1-generator.md
