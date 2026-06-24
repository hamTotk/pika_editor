# pika — プロジェクト指示

Windows 向け超軽量 Markdown/HTML エディタ。AIエージェントの出力物を確認・差分レビューする伴走ツール。
対応環境は **Windows 11 x64 のみ**、UI言語は **日本語のみ**、CLIコマンド名は `pika`。

ユーザーとのやり取り・コード内コメント・コミットメッセージはすべて **日本語**。

## ドキュメント階層（これが正典）

実装の入力は `docs/` の3層。下位ほど詳細で、迷ったら上位の意図に従う。

1. `docs/minimal-plan.md` — コンセプト・スコープの根拠
2. `docs/requirements.md` — **要件の正**（機能の可否はここで判断）。全14章＋各章「受け入れ基準」
3. `docs/design.md` — 設計（アーキテクチャ・モジュール・フロー・実装順序）
4. `docs/ui-design.md` — **UI視覚仕様の正典**（design.md 10章・requirements.md 11章の詳細化）。配色トークン・タイポ・状態記号（差分あり ±／新規 ◆／削除済み 取り消し線）・ファイルタイプアイコン・ステータス右下固定・差分トグルUI。モック実体は `docs/ui-mock.html`

**機能を足したくなったら、まず requirements.md 14章「やらないこと」を確認する。** 載っていなければ実装せず、要件改訂を提案する。ユーザーは「軽量さ＞開発効率」「MVPに含めない」判断を一貫して優先している。

## 設計原則（迷ったらこの優先順位／design.md 1章）

1. **データを失わない** — どの異常系でも復元可能に。退避スナップショットが最後の砦。index.json 破損時も退避を放棄せず objects の自己記述メタから復元する
2. **固まらない** — UIスレッドで200ms超ブロックしない。重い処理はワーカー（`TaskRunner`）へ
3. **軽い** — 未使用機能のコストはゼロ（遅延初期化・オンデマンド生成）。依存追加はバイナリサイズと天秤
4. **足さない** — 要件14章に無い機能は作らない
5. **速く作る** — 上記を満たす範囲で最も単純な実装を選ぶ

> **【Tauri フル刷新で改訂・2026-06-21】** 補助原則「ネイティブ優先」を廃し、全Web描画＋未信頼文書隔離へ
> 置換した（design doc 1章/2章/16章）。技術スタックは wx/C++/vcpkg/MSVC → Tauri/Rust/cargo/Vite。
> 正典は `docs/specs/2026-06-20-tauri-rewrite-architecture-design.md`。本書はその写し。

補助原則（design doc 1章。旧「ネイティブ優先」を置換）：**全Web描画**（UI は HTML/CSS/TS。CSS 変数トークンで質感を作る）／**未信頼コンテンツはアプリシェルから物理隔離**（文書由来 HTML/Markdown は権限ゼロの別 WebView で描画し Tauri API から分離）／**コアはUIを知らない**（`crates/pika-core` は Tauri/wry/WebView 非依存）／**ワークスペースを汚さない**（ユーザーのフォルダに pika のファイルを一切作らない）。

## アーキテクチャ（design doc 3章）

- レイヤー依存は一方向：**TS frontend（UI層・メインWebView）→ Tauri command/event 境界（アプリ層・src-tauri）→ pika-core（コアサービス層）→ プラットフォーム層（Win32/WebView2/FS）**。逆参照は禁止
- frontend → backend は `invoke('cmd', args)`、backend → frontend は `emit('event', payload)`（ワーカースレッドから）。`pika-core` は Tauri も WebView も知らない（command ハンドラが薄い境界）
- 未信頼文書（プレビュー）は**権限ゼロの別 WebView**＋custom protocol 直配信で描画し Tauri API から物理分離する
- コア公開APIは **`Result<T, PikaError>` 方式（thiserror）**。例外はモジュール内部に閉じる
- 文字列は UTF-8（Rust `String`）に統一し、Win32 境界で UTF-16 に変換

## リポジトリ構成（design doc 13章）

```
pika/
├── Cargo.toml                 cargo workspace（members: crates/pika-core, crates/pika-cli, src-tauri）
├── crates/
│   ├── pika-core/             UI/Tauri/wry 非依存コア（cargo test の決定論ゲート対象）
│   │   └── src/{document,workspace,watcher,diff,snapshot,render,search,settings,util,platform}/
│   └── pika-cli/              console subsystem の薄い CLI（--help/--version/引数検証/終了コード）
├── src-tauri/                 Tauri アプリ（pika-core 依存・GUI=pika.exe）
│   ├── Cargo.toml / tauri.conf.json / build.rs
│   ├── capabilities/          メイン=最小／プレビューWebView=ゼロ（ファイル不在＝権限ゼロ）
│   └── src/{main.rs, commands.rs, webview2.rs, ipc/, protocol/, cli.rs}
├── src/                       TS frontend（Vite）: index.html + {main.ts, ui/, editor/, diff/, preview/, theme/, a11y/, styles/}
├── package.json / tsconfig.app.json / vite.config.ts   frontend ビルド設定
├── assets/                    同梱 JS/CSS（mermaid/katex/highlight）, THIRD_PARTY_NOTICES
├── installer/                 Tauri bundler 設定＋ポータブルzip生成（sprint 7）
└── docs/
```

**ビルドターゲット**：`pika-core`（lib・Tauri/wry 非依存）／`pika`（GUI exe・src-tauri・windows subsystem）／`pika-cli`（console subsystem の薄いスタブ：`--help`/`--version`・引数検証・終了コード）。コアは GUI と CLI の両方からリンクする。

## ビルド／テスト

- 構成：Rust=cargo（Debug / Release）。Release は LTO・`opt-level="z"`・`strip`（軽量配布）。Rust の警告は **`warnings = "deny"`（workspace lints）でエラー扱い**。frontend=Vite+TypeScript（`strict`）
- 依存：Rust stable 1.96+・Node/npm（frontend）。Tauri/wry/WebView2 を使うため Windows 実機が必要
- コマンド（本フェーズの正）：
  - コアテスト（系統A・決定論ゲート）：`cargo test`（exit 0 で合格）
  - ビルド/型ゲート（系統A補）：`cargo build`（crates＋src-tauri・警告エラー扱い）＋ frontend 型チェック `npm run typecheck`（`tsc -p tsconfig.app.json`）
  - 監査（系統A監査）：`cargo audit`（CVE 追跡。CI ゲート）
- **完了の検証**：完了を主張する前に `cargo test` ＋ `cargo build` ＋ `npm run typecheck` を通す。GUI 実機・実測（IPC コスト・別WebView 権限ゼロ隔離・性能・a11y・WebView2 不在時）は系統C（`docs/acceptance.md`/`docs/acceptance-findings.md`）で確認する。編集直後の hook はファイル単位の即時検査（JSON 構文・C++ clang-format）、上記コマンドが全体整合の最終検査

### GUI 起動（実機確認）

1. **Vite を先に起動**：`npm run dev`（debug ビルドは devUrl `http://localhost:5173` を見る。未起動だと真っ白。release は埋め込み dist で不要）
2. **GUI ビルド**：`cargo build -p pika-app --bin pika`（exe＝`target/debug/pika.exe`）。bash では先に `export PATH="$USERPROFILE/.cargo/bin:$PATH"`。ビルド前に `taskkill //F //IM pika.exe`（実行中だと exe ロックでリンク失敗）
3. **起動（デタッチ）**：`cmd //c start "" target\debug\pika.exe <開くフォルダ>`

**テスト方針（design doc 12章）**：自動単体テストの対象は `crates/pika-core`。重点は diff（日本語の文字単位フォールバック・改行混在・空ファイル）・watcher のイベント合成/自己保存抑制/オーバーフロー再同期/rename正規化・snapshot の退避と容量管理・エンコーディング往復・render のサニタイズ。command 層/frontend TS は `cargo build`＋`npm run typecheck` のコンパイル/型成立を最低保証にする。UIの自動テストは初期版では持たない（`docs/acceptance.md` の手動チェックリストで代替）。性能は基準機・Release ビルドで Rust 側自己計測（QPC+psapi）しリリース前ゲートにする。

## 自動検査と規約（ハーネス）

- **編集直後の自動検査**：Edit/Write のたびに `.claude/hooks/post-edit-check.mjs` が走る。C++（.cpp/.h ほか）は `clang-format` の整形チェック（未導入時は素通し）、JSON は構文チェック（**厳密 JSON・コメント不可**）。整形差分・構文エラーは exit 2 で差し戻される。Rust=`cargo fmt`／TS フォーマッタは各 verify・CI 側で担保（hook は C++/JSON 専用）
- **コミットメッセージ規約** … [.claude/docs/commit.md](.claude/docs/commit.md)
- **命名・リポジトリ構成の正典** … [docs/specs/2026-06-20-tauri-rewrite-architecture-design.md](docs/specs/2026-06-20-tauri-rewrite-architecture-design.md) 13章（本書「リポジトリ構成」節はその写し。乖離したら design doc が正）

## 実装順序（design doc 14章）

1. **最薄のプラットフォーム証明ループをスプリント1に置く**：cargo workspace＋Tauri scaffold → フォルダを開く→ツリー→タブ→CM6 編集→保存。この段で (1) IPC コスト実測（プレビューHTMLを invoke vs custom protocol 直配信）・(2) 別WebView 権限ゼロ隔離の実証・(3) WebView2 不在時起動 を立証（design doc 15章 Open）
2. 中心体験の縦切り（開く→外部変更反映→差分→確認済み）を最優先で貫通させてから周辺機能を肉付け
3. 性能（起動時間・メモリ・**IPC ラウンドトリップ**）は各スプリント末に計測し劣化を即検知

## 実装時の判断ガイド（design doc 15章・ブレ防止）

- 機能/オプションを足したい → 要件14章を確認。無ければ実装せず要件改訂を提案
- 依存ライブラリを足したい → 配布サイズ30MB・保守状況を確認。**UI 依存は CodeMirror 等の必要最小・サイズと天秤**
- 未信頼文書（プレビュー）を扱う → **権限ゼロの別 WebView＋custom protocol 直配信**で隔離し Tauri API から物理分離（同一 WebView/iframe に置かない＝CVE-2024-35222 回避）
- UIスレッド（WebView）で I/O・重い処理を書きそう → **Rust 側スレッド**へ。IPC ラウンドトリップ自体もコスト（性能予算に計上）
- ユーザーデータを消す/上書きする → 退避スナップショットを先に書く（確認ダイアログより退避が先）
- ワークスペース内にファイルを書きたい → 禁止。データルート（既定 `%LOCALAPPDATA%\pika\`、ポータブル版は `./pika-data/`）へ

## 確定済みの主要判断（メモリと整合）

- プレビューの外部リソース取得＝既定オフ（オプトイン）／md・差分は JS有効（同梱信頼JS のみ・nonce＋CSP）・HTML は JS無効。CSP は custom protocol のレスポンスヘッダで強制（文書内 `<meta>` 依存にしない）
- 10MB以上はハッシュのみ記録（巨大ファイル閾値は sprint 6 の CM6 実測で確定＝要件2.2/5.4/9.2 TBD）／機密ファイル（.env 等）もハッシュのみ＋手動パージ・custom protocol からも配信拒否
- コード署名はしない（`docs/install.md` に SmartScreen 回避手順）
- `settings.toml` は読み取り専用（pika は書き戻さない）／正規表現は fancy-regex 第一候補（不足機能のみ pcre2）／`portable.txt` でデータルート切替
- 正規表現・サニタイズ・差分・監視・スナップショットは `crates/pika-core`（Tauri 非依存）に置き `cargo test` で固める
