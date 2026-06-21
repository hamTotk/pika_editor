# run-dev レポート — pika（Tauri フル刷新フェーズ）

- 対象: `C:/dev/pika_editor`（branch `main`）
- 最終ステータス: **done**（全7スプリント PASSED）
- 総ターン数: 10（Phase 2 の sprint ターン延べ）/ 総ターン上限: 21
- 退避ブランチ: `wip/cpp-core-snapshot`（旧 C++ 全ツリーを `11e94d7` に保全）
- 設計正典: `docs/specs/2026-06-20-tauri-rewrite-architecture-design.md`（Tauri 技術正典）

## スプリント結果表

| sprint | goal（要約） | ターン | 最終score | passed | 最終commit | 未達/残課題 |
|--------|-------------|--------|-----------|--------|-----------|------------|
| 1 | 旧C++退避＋Tauri scaffold＋最薄証明ループ＋canon改訂着手 | 1 | 96 | ✅ | `0e8a454` | IPC実測/権限ゼロ隔離/WebView2不在は系統C |
| 2 | watcher自前合成層（デバウンス/rename正規化/自己保存抑制/オーバーフロー再同期） | 1 | 92 | ✅ | `889a9f4` | オーバーフロー表面化の実機確認は系統C |
| 3 | 差分→確認済み（snapshot/diff/退避が最後の砦/確認済みフロー） | 1 | 93 | ✅ | `8a78c77` | SnapshotService 永続化は繰り越し（後述） |
| 4 | プレビュー権限ゼロ隔離/ammoniaサニタイズ/CSPヘッダ強制 | 2 | 90 | ✅ | `8f3a681` | Mermaid/KaTeX/highlight 実注入は系統C実アタッチ段へ据え置き |
| 5 | CLI二段構成/自前named pipe単一インスタンス/状態復元3分岐 | 2 | 100 | ✅ | `3fef265` | — |
| 6 | 巨大ファイル段階制/エンコーディング往復/検索置換＋canon確定 | 1 | 94 | ✅ | `a8a5778` | data high2（atomic_write/save退避）→sprint7で解消 |
| 7 | a11y全Web再構築/エッジケース/配布仕上げ/acceptance整備 | 2 | 94 | ✅ | `dd0afd6` | 残high2＋cargo audit/配布bundlerは系統C・後述 |

- sprint4・5・6・7 は WIP→区切りの複数ターンを経て合格（4: iter1 88→iter2 90 / 5: iter1 84→iter2 100 / 7: iter1 84→iter2 94）。sprint6 は iter1 で一発合格。
- 決定論ゲートは全ターンで `cargo test` ＋ `cargo build`（warnings=deny）＋ `npm run typecheck`（環境の癖で2→3コマンド運用）を exit 0 で通過。テスト改竄照合は全ターンでクリーン。tamper 照合（manifest baseline 比較）は全ターン UNCHANGED。

## 多角レビュー結果

review-profile.json の最終構成（全 conditional 中心・決定論ルーティング）:
`security / data / frontend-ui / product / performance / ux = conditional`、`backend-api = off`。
pika はローカル単一ユーザーの Windows デスクトップアプリで外部公開 API を持たないため backend-api を off とし、設計二大根幹リスク（未信頼コンテンツの物理隔離＝security／データを失わない＝data）を criteria キーワードで厚く拾う構成。

### sprint 6（巨大ファイル/エンコーディング/検索置換）— iter1 で合格
- 起動 reviewer: security / performance / data / frontend-ui / ux（5観点）
- critical = 0。high 5件:
  - **data ×2**: `atomic_write` の Windows 置換が `remove_file→rename` 2段でクラッシュ窓を持つ／`save_document` が破壊的上書き前に incoming 退避（SnapshotService）を経由しない（最上位原則「データを失わない」直撃）
  - **performance ×1**: `search/replace` が編集全文（最大50MB）を毎回 IPC 転送（置換は往復2倍）
  - **frontend-ui/ux ×各1**: 巨大ファイル仮想化ビューア・保存中断・decode_warning 開き直し・モード別検索置換が ipc.ts 契約止まりで live UI 未到達（spec の sprint7 配線委譲が明示済み）
- → data high2件は **sprint 7 で解消**（下記）。

### sprint 7（a11y/エッジケース/配布）— iter1 fail → iter2 pass
- iter1 起動 reviewer: security / performance / data / frontend-ui / ux / **product**（6観点・product は criteria 初起動）
  - **frontend-ui critical ×1**: ファイルツリーがキーボードのみで操作不能（矢印/Enter/Space ハンドラ皆無・tree に tabindex なし）＝中心体験の起点（openFile）にキーボードで到達不能。sprint7 自身の must「ARIA 全Web再構築」と req11.4/11.5 に正面衝突 → d_accept=0/d_review=0 で **passed=false（score 84）**
  - data reviewer が sprint6 持ち越しの解消を確認: `atomic_write` は `MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)` 単一アトミック置換へ／save は「退避が先・退避不能なら中断」を結線／`huge.rs` の閾値三者一致をコンパイル時 `const _` assert で単一源化
- iter2 起動 reviewer: performance / frontend-ui / ux（path ルーティング・security/product は criteria 既起動でスキップ・data はシグナルなし）
  - **critical = 0（全観点）**。frontend-ui の critical 解消を確認（roving tabindex・↑/↓/Home/End・Enter/Space で openFile・aria-expanded を本実装、`shortcuts.rs` を `src/shortcuts.ts` で 1:1 ミラーし main.ts へ配線）→ d_accept=100/d_review=100 で **passed=true（score 94）**
  - 残 high ×2（リリース非ブロック・系統C/次対応）: ①外部変更再描画でツリー roving フォーカスが先頭へ巻き戻る操作一貫性 ②Ctrl+F/H が実在しない「表示メニュー」へ誘導する行き止まり

## リリース前に残る繰り越し課題（系統C / release ゲート）

run-dev の自動ゲート（cargo test/build・typecheck・tamper）はすべて通過済み。以下は GUI 実機・実測・配布を要する系統C 項目で、リリース判断のゲートとする:

1. **SnapshotService の永続化**（sprint3 からの意図的繰り越し）— 退避が「最後の砦」である以上、インメモリ保持をリリース前に content-addressed object ストアへ永続化する必要がある（data reviewer 記録）
2. **live UI の open/save 完全差し替え** — `open_document`/`save_document` 経路への置換は本ランで部分結線（onSave は save_document 経由へ前進）。縮退通知/保存中断ダイアログ/仮想化ビューア/モード別検索置換の DOM 発火は系統C（acceptance TG/TF 行）で実アタッチ検証
3. **Mermaid/KaTeX/highlight 実注入**（sprint4 dead code）— 別WebView 実アタッチ段での結線
4. **cargo audit の CVE ゲート** — 実行環境に cargo-audit 未導入のため未実行（T-010/T-011 に記録）。リリース前に comrak(unsafe_HTML)/ammonia/Mermaid/KaTeX 等の攻撃面を advisory 照合
5. **配布 bundler**（要件13・should）— ユーザー単位インストーラー/ポータブルzip/エクスプローラー統合/About ライセンスは spec で「自動 verify に載せない」明記の意図的 deferral
6. sprint7 残 high2件（ツリー roving 巻き戻し・Ctrl+F/H 行き止まり）と各種 medium/low（acceptance に追跡記載済み）
7. 系統C 手動チェックリスト全体（IPC ラウンドトリップ実測・別WebView から invoke/__TAURI_INTERNALS__ 到達不能の実証・watcher オーバーフロー再同期・CM6 巨大ファイル体感・起動/メモリ/プレビュー初回・ナレーター/a11y・WebView2 不在時挙動）

## テスト規模（最終）

- `pika-core` 決定論テスト: **317件 PASS**（sprint6 で +61〔huge/range/encoding/search〕、sprint7 で +49〔notify_queue/diagnostic/nontext/view_state/shortcuts〕）
- `pika-cli` 11件 / `pika-app` 5件 PASS
- `cargo build`（crates＋src-tauri・warnings=deny）exit 0 / `npm run typecheck`（tsc strict）exit 0

## 停止理由・再開

- status = **done**（全 sprint 合格）。再開不要。
- 新たな要望で再実行する場合は `dev/` を退避してから `/run-dev` を再実行する。
- 次フェーズは系統C（`docs/acceptance.md` / `docs/acceptance-findings.md`）の GUI 実機・実測検証。上記「繰り越し課題」をリリースゲートとして消化する。

## コミット対応表

| sprint/iter | score | passed | commit |
|-------------|-------|--------|--------|
| 1 / 1 | 96 | ✅ | `0e8a454` |
| 2 / 1 | 92 | ✅ | `889a9f4` |
| 3 / 1 | 93 | ✅ | `8a78c77` |
| 4 / 1 | 88 | ❌ | `d528987` |
| 4 / 2 | 90 | ✅ | `8f3a681` |
| 5 / 1 | 84 | ❌ | `0a5fa9b` |
| 5 / 2 | 100 | ✅ | `3fef265` |
| 6 / 1 | 94 | ✅ | `a8a5778` |
| 7 / 1 | 84 | ❌ | `3ff44c2` |
| 7 / 2 | 94 | ✅ | `dd0afd6` |
