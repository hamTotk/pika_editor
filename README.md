# Pika Editor

Windows 向けの超軽量 Markdown/HTML エディタ。外部 AI エージェントが生成・編集したファイルを、リアルタイムで確認・差分レビューするための伴走ツールです。

## これは何か

Claude Code のような AI エージェントに Markdown やレポート HTML を書かせていると、「変更を見るためだけに VS Code を開く」のが煩わしくなります。Pika Editor は、フォルダを開いておくだけで外部からのファイル変更を即座に検知し、前回確認した時点からの差分をツリー上に表示するビューア寄りの軽量エディタです。VS Code や IDE の代替ではなく、「見る・確認する・少し直す」に機能を絞っています。

## 主な機能

- フォルダ / ファイルを開く、ツリー表示（変更ありファイルを未読バッジで表示）
- Markdown / HTML のリアルタイムプレビュー（GFM・Mermaid・KaTeX 数式・シンタックスハイライト対応）
- 外部変更の即時反映（AI エージェントの書き込みを watcher で検知し、画面が飛ばずに反映）
- 「前回確認した時点 → 現在」の累積差分を赤緑＋記号で表示し、確認済みにする操作でベースラインを更新
- 確認済み時点へのファイル単位の巻き戻し、誤編集からの復元用スナップショット
- CLI (`pika <path>`) からの起動、シングルインスタンス動作でエージェントのワークフローに組み込める
- インストーラー版 / ポータブル版の両方を配布（コード署名なし・自動更新なし）

## 技術スタック

- Backend: Rust + Tauri 2（Chromium を同梱せず OS の WebView2 を使うため配布サイズが小さい）
- Frontend: TypeScript（vanilla, フレームワーク非採用）+ Vite + CodeMirror 6
- Markdown 変換: comrak（GFM）→ ammonia（サニタイズ）/ 差分: similar（Myers 法 + 日本語向け文字単位フォールバック）

## アーキテクチャ

cargo workspace を 3 つのクレートに分けています。

- `crates/pika-core` — UI・Tauri に依存しないコアロジック（差分・監視・スナップショット・レンダリング等）。`cargo test` で自動検証する
- `crates/pika-cli` — `--help` / `--version` 用の薄い CLI スタブ
- `src-tauri` — Tauri 本体。`pika-core` に依存する唯一の橋渡し層

もっとも気を使った設計判断は、AI が生成した未信頼な HTML/Markdown を安全に表示することです。プレビューは権限ゼロの別 WebView で描画し、custom protocol 経由でのみ配信することで、アプリ本体の Tauri API から物理的に切り離しています（Windows の同一オリジン iframe に関する既知の脆弱性を踏まえた設計）。

## ビルド & 実行

Windows 11 x64 + WebView2 Runtime が前提です。

```bash
npm install
npm run dev                          # Vite dev server（先に起動しておく）
cargo build -p pika-app --bin pika   # target/debug/pika.exe が生成される
```

テスト・型チェック:

```bash
cargo test        # pika-core のユニットテスト
cargo build       # 警告をエラー扱いにしたビルドゲート
npm run typecheck # フロントエンドの型チェック
```

## 開発について

このプロジェクトは Claude Code をメインの実装者として、要件定義（`docs/requirements.md`）・設計（`docs/design.md`）・UI 仕様（`docs/ui-design.md`）を先に固めたうえで、issue 駆動で機能追加を進めています。まとまった量の実装は自動スプリントループ（複数の AI エージェントが実装 → レビュー → 評価を繰り返す仕組み）に任せ、人間はレビューとマージ判断に専念する形で開発しました。「AI に何をどこまで任せるか」を明文化した運用ルールは `CLAUDE.md` と `.claude/skills/` に残しています。

もともと wxWidgets/C++ で作り始めましたが、軽量さと保守性の両立を再検討した結果、Tauri + Rust への全面刷新を行っています。

## ライセンス

MIT License
