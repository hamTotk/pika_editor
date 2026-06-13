# pika — プロジェクト指示

Windows 向け超軽量 Markdown/HTML エディタ。AIエージェントの出力物を確認・差分レビューする伴走ツール。
対応環境は **Windows 11 x64 のみ**、UI言語は **日本語のみ**、CLIコマンド名は `pika`。

ユーザーとのやり取り・コード内コメント・コミットメッセージはすべて **日本語**。

## ドキュメント階層（これが正典）

実装の入力は `docs/` の3層。下位ほど詳細で、迷ったら上位の意図に従う。

1. `docs/minimal-plan.md` — コンセプト・スコープの根拠
2. `docs/requirements.md` — **要件の正**（機能の可否はここで判断）。全14章＋各章「受け入れ基準」
3. `docs/design.md` — 設計（アーキテクチャ・モジュール・フロー・実装順序）

**機能を足したくなったら、まず requirements.md 14章「やらないこと」を確認する。** 載っていなければ実装せず、要件改訂を提案する。ユーザーは「軽量さ＞開発効率」「MVPに含めない」判断を一貫して優先している。

## 設計原則（迷ったらこの優先順位／design.md 1章）

1. **データを失わない** — どの異常系でも復元可能に。退避スナップショットが最後の砦。index.json 破損時も退避を放棄せず objects の自己記述メタから復元する
2. **固まらない** — UIスレッドで200ms超ブロックしない。重い処理はワーカー（`TaskRunner`）へ
3. **軽い** — 未使用機能のコストはゼロ（遅延初期化・オンデマンド生成）。依存追加はバイナリサイズと天秤
4. **足さない** — 要件14章に無い機能は作らない
5. **速く作る** — 上記を満たす範囲で最も単純な実装を選ぶ

補助原則：**ネイティブ優先**（WebView2 を使ってよいのはプレビューと差分表示だけ）／**コアはUIを知らない**（`core/`・`util` は wxWidgets 非依存）／**ワークスペースを汚さない**（ユーザーのフォルダに pika のファイルを一切作らない）。

## アーキテクチャ（design.md 2章）

- レイヤー依存は一方向：**UI層 → アプリケーション層 → コアサービス層 → プラットフォーム層**。逆参照は禁止
- コア→UI への通知はコールバック（`std::function`）またはイベントキュー経由のみ。コアが wx のウィンドウクラスを直接触らない
- コアサービス同士は原則独立（連携はアプリケーション層が仲介）。例外は `core/settings` が `settings.toml` 監視に `core/watcher` を直接使う1点のみ
- コア公開APIは **`Result<T>` 方式**。例外はモジュール内部に閉じる
- 文字列は `std::string`（UTF-8）に統一し、Win32境界で UTF-16 に変換

## リポジトリ構成（design.md 12章）

```
pika/
├── CMakeLists.txt           ルート。vcpkg manifest モード
├── vcpkg.json               依存: wxwidgets(3.3+), md4c, dtl, pcre2, zstd, xxhash, toml11, gtest
├── vcpkg-configuration.json builtin-baseline を特定コミットSHAにピン留め（再現ビルド）
├── src/
│   ├── app/                 main, CLI(-g パース), データルート解決, 単一インスタンス
│   ├── controller/
│   ├── ui/
│   ├── core/{document,workspace,watcher,diff,snapshot,render,search,settings,ipc}/
│   └── util/
├── assets/                  プレビューテンプレート, css, mermaid/katex/highlight, vendor.lock, THIRD_PARTY_NOTICES
├── tests/                   core/util の単体テスト（gtest）
├── installer/               Inno Setup スクリプト＋ポータブルzip生成
└── docs/
```

**ビルドターゲット**：`pika_core`（静的lib・wx非依存部＝`core/`＋`util`）／`pika`（GUI exe）／`pika.com`（コンソールの薄いスタブ：`--help`/`--version`・引数検証・終了コード）／`pika_tests`。コアは exe とテストの両方からリンクする。

## ビルド／テスト

> CMakeLists.txt はまだ未作成。スキャフォールド後、下記コマンドを正とする。

- 構成：Debug / Release。Release は `/MT`（静的CRT）・`/O2`・LTCG。警告レベル `/W4`・警告はエラー扱い（外部ヘッダ除く）
- 依存：CMake + MSVC + vcpkg（manifest モード）。`vcpkg-configuration.json` の baseline を SHA ピン留めして再現ビルド
- 想定コマンド：
  - 構成：`cmake --preset x64-release`（プリセットは CMakeLists.txt 作成時に定義）
  - ビルド：`cmake --build --preset x64-release`
  - テスト：`ctest --preset x64-release`（gtest）

**テスト方針（design.md 13章）**：自動単体テストの対象は `core/`・`util`。重点は diff・watcher のイベント合成/自己保存抑制・snapshot の退避と容量管理・エンコーディング往復・render のサニタイズ。UIの自動テストは初期版では持たない（`docs/acceptance.md` の手動チェックリストで代替）。性能は基準機・Releaseビルドで自動計測しリリース前ゲートにする。

## 実装順序（design.md 14章）

1. **技術リスク3点のスパイクをスプリント1に置く**：(a) wx＋Scintilla＋WebView2 が1ウィンドウ共存で起動500ms＋`TrySuspend`再表示300ms、(b) watcher のイベント合成・自己保存抑制・バッファオーバーフロー再同期、(c) WebView2 の仮想ホスト＋サニタイズ＋JS有効/無効の高速切替の順序保証
2. 中心体験の縦切り（開く→外部変更反映→差分→確認済み）を最優先で貫通させてから周辺機能を肉付け
3. 性能（起動時間・メモリ）は各スプリント末に計測し劣化を即検知

## 実装時の判断ガイド（design.md 15章・ブレ防止）

- 機能/オプションを足したい → 要件14章を確認。無ければ実装せず要件改訂を提案
- 依存ライブラリを足したい → 配布サイズ30MB・静的リンク可否・保守状況を確認。**UI系の依存追加は原則禁止**
- WebView2 でUIを作る方が楽に見える → ネイティブ優先に立ち返る（WebView2 はプレビュー・差分のみ）
- UIスレッドでI/Oを書きそう → `TaskRunner` へ。「小さいファイルだから」はネットワークドライブで破綻する
- ユーザーデータを消す/上書きする → 退避スナップショットを先に書く（確認ダイアログより退避が先）
- ワークスペース内にファイルを書きたい → 禁止。データルート（既定 `%LOCALAPPDATA%\pika\`、ポータブル版は `./pika-data/`）へ

## 確定済みの主要判断（メモリと整合）

- プレビューの外部リソース取得＝既定オフ（オプトイン）／md・差分は JS有効（サニタイズ＋CSP）・HTML は JS無効
- 10MB以上はハッシュのみ記録／機密ファイル（.env 等）もハッシュのみ＋手動パージ
- コード署名はしない（`docs/install.md` に SmartScreen 回避手順）
- `settings.toml` は読み取り専用（pika は書き戻さない）／PCRE2 同梱／`portable.txt` でデータルート切替
