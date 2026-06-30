---
name: ref-pika-workflow
description: pika_editor の機能追加・修正・PR・リリース作業で発動。「pika 開発フロー」「issue 駆動」で参照する pika の開発フロー ref。
kind: essence
user-invocable: true
---

# pika 開発フロー (ref)

pika_editor（Windows 向け超軽量 Markdown/HTML エディタ・Tauri/Rust/TS）の開発作業を一望する運用辞書。**何を作るか**の正典は `docs/`、**プロジェクト原則**は `CLAUDE.md`（自動で文脈に入る）。本 ref はそれらを前提に、**機能追加・修正を issue 駆動でどう回すか**のフローと、CLAUDE.md に書ききれない**運用上の判断軸**に集中する。

## 着手前に必ず効く門

- **機能を足したくなったら、まず `docs/requirements.md` 14章「やらないこと」を確認**。載っていなければ実装せず要件改訂を提案する。ユーザーは「軽量さ＞開発効率」「MVP に含めない」を一貫優先（理由: スコープ膨張が pika の存在意義＝超軽量を殺す）
- **完了を主張する前に系統A 3点を通す**: `cargo test` ＋ `cargo build`（警告 deny）＋ `npm run typecheck`。視覚・実挙動の確認は系統C（手動 acceptance）でしか取れない
- **サブエージェント/generator の報告を鵜呑みにしない**。メイン側で `cargo test`/`build`/`typecheck` を独立再実行し、必要なら別 agent の実機観察で裏取りする（理由: 過去に generator の虚偽報告で「通った」が実際は失敗していた）
- **ユーザーデータを消す/上書きする前に退避スナップショットを先に書く**（確認ダイアログより退避が先）。設計原則の優先順位は **データを失わない ＞ 固まらない ＞ 軽い ＞ 足さない ＞ 速く作る**

## ドキュメント正典（迷ったら上位の意図に従う）

1. `docs/minimal-plan.md` — コンセプト/スコープの根拠
2. `docs/requirements.md` — **要件の正**（機能の可否はここ）。全14章＋各章「受け入れ基準」。14章「やらないこと」が機能追加の門
3. `docs/design.md` — 設計（アーキ・モジュール・実装順序）
4. `docs/ui-design.md` — **UI視覚仕様の正典**。モック実体 `docs/ui-mock.html`
- 技術スタックの正典は `docs/specs/2026-06-20-tauri-rewrite-architecture-design.md`（Tauri 刷新後）
- 系統C の所見台帳 `docs/acceptance-findings.md`、手動チェックリスト `docs/acceptance.md`

## issue 駆動の開発フロー（新方針・本 ref の核）

remote は `origin = github.com/hamTotk/pika_editor`（private）。アイデアは一度 issue に落としてから個別対応する。

1. **アイデア → issue**（起票はケースバイケース）
   - 大きい構想・要件に触れる変更: Claude と壁打ち（run-brainstorm）→ 要件14章照合・粒度調整 → `gh issue create`、最終文面はユーザー承認
   - 小さい修正・明確なバグ: ユーザーが直接起票し、Claude は番号を受け取って実装に入る
   - どちらでも issue 本文に **背景/なぜ・受け入れ基準・関連する要件章番号（あれば）** を書く（後で着手判断と要件整合に効く）
2. **着手**: issue を選ぶ → **計画を提示して GO を得てから実装**。要件改訂が絡むなら計画より先に改訂提案を出す
3. **ブランチ**: `<type>/<説明>[-YYYY-MM-DD]` を main から切る（例: `feat/shell-filetype-icons`・`fix/nsis-bundle-config-2026-06-29`）。type はコミット type と揃える
4. **実装**: 規模で手段を選ぶ（下の「実装手段の判断軸」）
5. **検証**: 系統A を通す → UI/実挙動に触れたら系統C 手動 acceptance（所見は `docs/acceptance-findings.md` へ）
6. **コミット**: 日本語・**3行以内**・1行目 `type: 要約`（type ∈ feat/fix/docs/refactor/test/perf/build/ci/chore）。2-3行目は **なぜ**（what は diff で分かる）。要件/設計変更は章番号を本文参照。必須フッターはなし
7. **PR**: `gh pr create`（本文に `Closes #N`）→ `/review <PR#>` ＋ `codex review --background` の二重レビュー → 確定指摘を反映 → **メイン側で独立再検証**
8. **マージはユーザーが実行**（二重レビュー通過後）。Claude はセルフマージしない（理由: 最終ゲートを人に残す現状の慎重運用）

## 実装手段の判断軸

| 状況 | 手段 |
|---|---|
| 小〜中規模で自分で完結できる | メインで自分が実装（**できる実装を勝手に丸投げしない**） |
| substantial（多ファイル・品質を作り込む） | eval-loop（run-eval-loop・閾値80・1イテレ=1スプリント・評価は全体完成度） |
| プロダクト要望から多スプリント縦断 | run-dev |
| 大規模/並列/広範探索 | dev-generator 等へ委譲（Agent から dev-generator は直接起動不可・Skill 経由） |

- 確認/入力ダイアログは**必ず自前モーダル**（`confirmModal`/`promptText`）。pika の WebView2 では `window.confirm`/`prompt` がダイアログを出さず即値を返す（confirm≒true）ので、素通りで黙ってデータ損失する
- ワークスペース内にファイルを書かない。データルートは既定 `%LOCALAPPDATA%\pika\`、ポータブル版は `./pika-data/`

## 検証ゲート（系統A / 系統C）

- **系統A（決定論・自動ゲート）**: `cargo test`（pika-core・exit 0 で合格）/ `cargo build`（crates＋src-tauri・`warnings="deny"`）/ `npm run typecheck`（`tsc -p tsconfig.app.json`）/ `cargo audit`（CVE）
- **系統C（手動・実機）**: GUI を起動して acceptance。視覚・IPC コスト・別WebView 権限ゼロ隔離・性能・a11y・WebView2 不在時はここでしか取れない
- 編集直後 hook（`.claude/hooks/post-edit-check.mjs`）は C++ clang-format / JSON 構文の即時検査（exit 2 で差し戻し）。Rust fmt・TS フォーマッタは各 verify/CI 側
- **CI は現状機能していない**: `.github/workflows/ci.yml` は Tauri 刷新前の C++/CMake 雛形のまま、`build-test` は `if: false` で無効化。**Rust/Tauri 向け CI（cargo test/build/typecheck/audit）の再建は最初の issue 候補**。それまで系統A はローカル手動が正

<important if="pika を実機起動して系統C 確認するとき">
## GUI 実機起動

1. **`npm run dev` を先に起動**（debug ビルドは devUrl `http://localhost:5173` を見る。未起動だと真っ白）。release で埋め込み dist を使うなら **`--features custom-protocol` 必須**（漏れると release でも devUrl を見て真っ白）
2. **ビルド**: `cargo build -p pika-app --bin pika`（exe = `target/debug/pika.exe`）。bash では先に `export PATH="$USERPROFILE/.cargo/bin:$PATH"`。**ビルド前に `taskkill //F //IM pika.exe`**（実行中だと exe ロックでリンク失敗）
3. **起動（デタッチ）**: `cmd //c start "" target\debug\pika.exe <開くフォルダ>`（PowerShell が deny される環境のため cmd 経由）
</important>

## Gotchas

- **`window.confirm`/`prompt` は素通り** → WebView2 で常時 true 相当。確認/入力は自前モーダル必須
- **release/ポータブルで真っ白** → `--features custom-protocol` 漏れ（devUrl を見て `ERR_CONNECTION_REFUSED`）
- **リンク失敗** → pika.exe 実行中の exe ロック。先に taskkill
- **generator 報告の鵜呑み** → メイン側で系統A 独立再実行＋実機観察で裏取り
- **機能を足す前に要件14章** → 「やらないこと」に該当しないか確認、なければ要件改訂提案が先
- **CI は当てにしない** → 陳腐化中。緑/赤に関係なくローカル系統A が正

## Additional resources

- `CLAUDE.md` — プロジェクト原則・設計原則・確定済み判断（自動で文脈に入る）
- `docs/` — 上記「ドキュメント正典」の実体
- `.claude/docs/commit.md` — コミットメッセージ規約の正
- `.github/ISSUE_TEMPLATE/` ・ `.github/PULL_REQUEST_TEMPLATE.md` — issue/PR の入力雛形
