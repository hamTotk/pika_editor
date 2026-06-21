# turn 7-1 generator report — sprint 7（a11y 全Web再構築・エッジケース・配布の仕上げ）

## 概要

sprint 7 は最終スプリント。design doc 17章（ARIA 全Web再構築）・18章（WebView2 不在時）・19章（移植
チェックリスト残項目）・ui-design 15章（5状態）を仕上げる。spec の検証戦略どおり、UI に依存しない
**決定論部分を pika-core に集約して cargo test で固め**、DOM 描画・実機 a11y は系統C（acceptance.md）へ寄せた。
前ターン eval（turn-6-1）の feedback_structured の **high（data）2 件を本スプリントで是正**し、low（閾値単一源化）も対応した。

新規 pika-core 決定論テスト **49 件**（268 → 317）。全 verify gate（cargo test / cargo build / cargo fmt --check /
npm run typecheck）が exit 0。

## 変更ファイル一覧

### pika-core（新規モジュール・決定論ゲート対象）
- `crates/pika-core/src/notify_queue.rs`（新規）— 通知バーキュー運用（要件11.1・must1）。優先順位/最大3本+他N件/
  同一ファイル同一種別合体/タブ固有vsグローバル/自動消滅条件。テスト 11 件。
- `crates/pika-core/src/diagnostic.rs`（新規）— 診断ログ方針（要件12.3・must5）。レベル判定（既定 warn 以上）/
  ログフォルダパス解決/`LogLine`（本文フィールドを持たない型）/5MB×3世代ローテーション計画。テスト 7 件。
- `crates/pika-core/src/nontext.rs`（新規）— 非テキスト/FSエッジ縮退（要件12.1/12.2・must4）。拡張子分類/
  画像寸法プリチェック（6000万px 超は外部誘導）/FSエッジ縮退方針。テスト 10 件。
- `crates/pika-core/src/view_state.rs`（新規）— ビュー別5状態（ui-design 15章・must3）。Ideal/Empty/Loading/
  Partial/Error 解決・Empty 3分岐・Partial 縮退理由+手動再有効化可否。テスト 9 件。
- `crates/pika-core/src/shortcuts.rs`（新規）— 主要ショートカット表（要件11.2・should）。Ctrl+Enter 誤爆防止/
  代替割当（Alt+Down/Up・Ctrl+Shift+E）。テスト 11 件。
- `crates/pika-core/src/lib.rs` — 上記5モジュールを登録・モジュール doc 更新。
- `crates/pika-core/src/huge.rs` — 10MB 閾値の三者一致を **`const _: () = { assert!(...) }`** でコンパイル時担保
  （eval low data: 単一源化）。+1 既存テストに影響なし。

### src-tauri（配線・データ損失系是正）
- `src-tauri/src/diagnostic.rs`（新規）— pika-core::diagnostic 委譲のログ追記/ローテーション実行・
  `log_folder_path` command（「メニューからログフォルダを開ける」・要件12.3）。
- `src-tauri/src/document.rs` — (a) **atomic_write を `MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`
  の単一アトミック置換へ変更**（eval high data#1: remove_file→rename の2段クラッシュ窓を廃止）。(b) **save_document に
  保存前 incoming 退避**を結線（eval high data#2: 退避が先・取れなければ保存中断）。(c) save/open 失敗経路に診断ログ。
- `src-tauri/src/snapshot.rs` — `SnapshotService::stash_incoming_before_overwrite`（破壊的上書き前にディスク現内容を
  incoming 退避・退避不能/不要は判定でスキップ・退避失敗は Err で保存中断）。
- `src-tauri/src/main.rs` — diagnostic モジュール登録・`log_folder_path` を invoke_handler へ追加。

### frontend（ARIA・通知・5状態・画像・CSS）
- `src/a11y/index.ts` — **F6/Shift+F6 ペイン間フォーカス循環**（自前フォーカスマネージャ・must2）・ランドマーク確実化。
- `src/ui/notifications.ts` — NoticeQueue を pika-core::notify_queue と同規則で実装（最大3本+他N件・優先順位・合体・
  タブ固有/グローバル・自動消滅・actions/閉じる描画）。旧 notify() は後方互換維持。
- `src/ui/status.ts` — `renderStatus`（差分あり件数を **aria-label** 化・要件11.5）。
- `src/ui/viewstate.ts`（新規）— 5状態文言・DegradeFlagsDto から縮退理由組立（pika-core 同規則）。
- `src/ui/image.ts`（新規）— 画像簡易ビュー（fit/actual）・「既定アプリで開く」誘導。
- `src/main.ts` — `initA11y()` 呼び出し・activateTab で `notices.setActiveTab`（タブ切替で通知切替）・openFile で
  画像種別検知・`notifyDegrade`（Partial 縮退を通知バー提示）を結線（viewstate/image を live UI から到達可能に）。
- `src/ipc.ts` — `logFolderPath` バインディング追加。
- `src/styles/app.css` — 通知 actions/閉じる/他N件・画像ビュー・「既定アプリで開く」誘導・
  `@media (forced-colors: active)` で独自トークン降格（should: forced-colors 追従）。

### docs（系統C 整備・must6）
- `docs/acceptance.md` — Tauri フェーズに **TG セクション（TG1〜TG10）** を新設（ARIA/F6/通知バー/5状態/画像/
  FSエッジ/診断ログ/ショートカット/forced-colors/配布）。各項目に requirements/design doc 節番号を併記。
- `docs/acceptance-findings.md` — **T-010**（sprint 7・決定論側の網羅・データ損失是正の根拠・系統C 残項目）を追記。

## must / should criteria 実装状況

### must
1. **通知バーキュー運用（pika-core/frontend）** — ✅ `notify_queue.rs`（11テスト）で優先順位/最大3本+他N件/同一
   ファイル同一種別合体/タブ固有vsグローバル/種類別自動消滅を決定論で解決。frontend `notifications.ts` が同規則で
   描画、`activateTab` で setActiveTab を結線。要件11.4 の「4件以上→3本+他N件」「同一ファイル衝突1本」「タブ切替で
   表示切替」をテストで観測。
2. **ARIA 全Web再構築（frontend）** — ✅ ツリー/タブ/通知（role=status/aria-live）/ステータス（差分あり件数 aria-label）
   は index.html＋ui/* で付与済み。**F6/Shift+F6 ペイン間フォーカス循環を `a11y/index.ts` で自前実装**（capture
   フェーズ・hidden ペインスキップ）し main.ts で初期化。差分の色非依存（+/-記号+下線）は既存維持。DOM 読み上げ実機は
   系統C（TG1/TG2）。
3. **ビュー別5状態（frontend・決定論は pika-core）** — ✅ `view_state.rs`（9テスト）で Ideal/Empty/Loading/Partial/
   Error 解決・**Empty 3分岐**（フォルダ未オープン/検索0件/消化後で文言が変わる）・Partial 縮退理由+手動再有効化可否。
   frontend `viewstate.ts` が文言提示、`notifyDegrade` で Partial を通知バーへ。描画は系統C（TG4）。
4. **非テキスト/エッジケース（決定論は pika-core）** — ✅ `nontext.rs`（10テスト）で拡張子分類・**画像寸法プリチェック
   （6000万px 超はデコードせず外部誘導）**・非対応バイナリ・FSエッジ縮退方針（読み取り専用/アクセス権なし/ネットワーク
   ドライブ/クラウドプレースホルダ/ワークスペース消失）を決定論で算定。frontend `image.ts` が簡易ビュー/外部誘導描画。
   実デコード/実描画は系統C（TG5/TG6）。
5. **診断ログ** — ✅ `diagnostic.rs`（7テスト）で error/warn/info・既定 warn 以上・`LogLine`（本文フィールドを持たない
   型でユーザー内容を構造的に書けない）・5MB×3世代ローテーション計画。src-tauri `diagnostic.rs` が追記/ローテーション
   実行・`log_folder_path` command で「メニューからログフォルダを開ける」。save/open 失敗経路から `record` を実使用。
6. **docs/acceptance.md / acceptance-findings.md 整備** — ✅ acceptance.md に TG1〜TG10（requirements/design doc 節番号
   併記）、acceptance-findings.md に T-010 を追記。IPC ラウンドトリップ/別WebView 到達不能/watcher オーバーフロー/CM6
   巨大ファイル/性能/ナレーター/WebView2 不在時/受け入れ基準写経は既存 TA〜TF＋TG＋TB に集約済み。
7. **cargo test / cargo build / typecheck exit 0・cargo audit** — ✅ test 317+11+5 PASS・build exit 0・typecheck exit 0・
   fmt clean。**cargo audit は本環境に未導入**（sprint 7 の verify 配列は `cargo test`/`cargo build` のみ）。turn report
   に記録し系統C/CI 安定段で CVE ゲートに載せる旨を T-010 に明記（テスト緩和ではなく環境制約の記録）。

### should
- ✅ 主要ショートカット表（`shortcuts.rs` 11テスト・誤爆防止/代替割当を決定論テスト）。
- ✅ forced-colors/テキストスケール（app.css に `@media (forced-colors: active)` でトークン降格・差分は色非依存維持）。
- 部分: Tauri bundler 配布（実インストーラー・エクスプローラー統合・About ライセンス）はリリース準備として系統C/別途
  （TG10）。テーマの OS 追従は既存 theme で維持。

### 前ターン feedback_structured 対応（turn-6-1-eval）
- **high data#1（atomic_write 非アトミック窓）** — ✅ `MoveFileExW` 単一置換へ是正。
- **high data#2（保存前 incoming 退避漏れ）** — ✅ `stash_incoming_before_overwrite` を save_document へ結線。
- **high performance（50MB 検索/置換 IPC）** — acceptance（TF/性能 TB）で IPC 実測項目化済み（系統C）。本スプリントは
  決定論側に集中し構造変更は見送り（spec 検証戦略の deferral）。
- **high frontend-ui/ux（保護導線の到達性）** — 一部結線（画像種別/degrade 通知/通知キュー/F6/diagnostic）。open/save の
  新コマンド経路への全面差し替えは CM6 ライブフロー大改修のため、spec どおり実描画は系統C（TG）に登録。
- **low data（閾値三者一致）** — ✅ huge.rs に `const _: () = { assert!(...) }` でコンパイル時担保。

## テスト化できなかった criteria とその理由

- **ARIA 読み上げ・F6 実フォーカス遷移・通知バー実描画・5状態の実遷移・画像実デコード・診断ログ実出力**: DOM/WebView2/
  ナレーター/実 FS が要るため系統C（acceptance.md TG1〜TG10）。決定論判定（キュー解決・状態解決・寸法プリチェック・
  ローテーション計画・ショートカット解決）は cargo test で観測可能にした（UI 非依存部分の最大化＝spec 検証戦略）。
- **Tauri bundler 実配布・エクスプローラー統合・ジャンプリスト実機**: リリース準備で系統C（TG10）。
- **cargo audit**: 本環境に未導入かつ verify 配列対象外。テスト無効化ではなく環境制約として記録。

## 自己実行した verify 結果

- `cargo test`（全 workspace）: pika-core **317** passed / pika-cli **11** passed / pika-app **5** passed / failed 0。
- `cargo build`（crates＋src-tauri・警告エラー扱い）: **exit 0**。
- `cargo fmt --check`: **clean**（新規 Rust ファイルは cargo fmt 適用済み）。
- `npm run typecheck`（tsc -p tsconfig.app.json）: **exit 0**。

（合否判定の正本は run-dev が別途実行する結果。上記は自己確認。）

DONE: C:/dev/pika_editor/dev/turns/turn-7-1-generator.md
