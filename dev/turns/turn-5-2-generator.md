# turn-5-2 generator report — sprint 5 修正ターン（CLI 二段構成・単一インスタンス・状態復元）

iteration 2。eval（turn-5-1-eval.json）は score=84・passed=false（全 must 充足・全次元足切り以上だが
should 0/3 で score<90）。本ターンは **feedback_structured を high → medium → low の順に対応**し、
**should 3 件を実装**して score を押し上げた。テストの緩和・スキップ・無効化はしていない。

sprint goal（再注入）: pika-cli.exe（console・--help/--version/引数検証/終了コード）＋pika.exe（GUI）の二段構成、
自前 named pipe による単一インスタンス＋引数転送（受信引数は core 検証層で再正規化）、-g パース、
state.json アトミック書込＋復元3分岐、最近使った項目＋ジャンプリストを結線する（design doc 15章-9）。

## 変更ファイル一覧

### pika-core（決定論ゲート対象・新規/拡張）
- `crates/pika-core/src/hashing.rs`（新規）— 内容ハッシュ共通規則 `hash_normalized_lf`（LF 正規化＋XxHash64 seed=0）を
  一箇所へ集約。従来 commands.rs / state_store.rs に重複実装され「同一規則」をコメントで約束していたドリフト源を解消。
  `HUGE_FILE_THRESHOLD_BYTES`(10MB) も定義。tests 6 件。
- `crates/pika-core/src/recent.rs`（新規）— 最近使った項目 `RecentList`（LRU・大文字小文字無視の重複排除・各20件上限・
  空/空白無視）。ジャンプリストの並び/重複/上限の決定論ロジック（要件10.2）。tests 6 件。
- `crates/pika-core/src/state.rs`（拡張）— `AppState.recent` フィールド追加（`#[serde(default)]` で後方互換）・
  `AppState::active_tab_path()`（active_tab インデックスをパスへ解決＝復元後の再アクティブ化用）。tests +2 件。
- `crates/pika-core/src/lib.rs`（拡張）— hashing / recent モジュール登録。

### src-tauri（薄い橋渡し層・compile/型ゲート）
- `src-tauri/src/jumplist.rs`（新規）— `SHAddToRecentDocs(SHARD_PATHW)` で OS の Recent ジャンプリストへ登録
  （`ICustomDestinationList` のカスタムカテゴリは要件外＝足さない・YAGNI）。並び/重複/上限は core 委譲。
- `src-tauri/src/state_store.rs`（拡張）— `RestoreOutcome.active_path` 追加・`probe_path` を metadata len 先読みにし
  **10MB 以上は全量読込せず Same 扱い**（起動ホットパス保護）・`save` を **fsync(sync_all)＋PID サフィックス tmp** で完全化・
  `LoadForUpdate`/`load_for_update`（recent の read-modify-write 用）・core hashing へ切替（重複削除）。
- `src-tauri/src/commands.rs`（拡張）— `hash_content`（タブ content_hash 算出）・`note_recent`（recent 追記＋ジャンプリスト反映・
  未知/破損は保全）・`path_kind`（存在/種別判定）command 追加。`save_app_state` は **recent をディスク既存値で保持**
  （note_recent が単独 owner・二重管理回避）。`RestoreOutcomeDto.active_path` 追加。重複 hash 関数を削除。
- `src-tauri/src/main.rs`（拡張）— jumplist モジュール登録・hash_content/note_recent/path_kind command 登録。
- `src-tauri/Cargo.toml`（拡張）— windows-sys に `Win32_UI_Shell`（SHAddToRecentDocs）追加。

### frontend（型ゲート）
- `src/editor/index.ts`（拡張）— `EditorHandle.getCursor()`/`getScrollTop()`（実カーソル/スクロール収集）・
  `scrollToLine()`（復元時の行スクロール）を追加。`CursorPos` 型。
- `src/ipc.ts`（拡張）— `hashContent`/`noteRecent`/`pathKind` ラッパ・`RecentList` 型・`RestoreOutcome.active_path`・
  `AppState.recent`。
- `src/main.ts`（拡張）— OpenTab に contentHash/cursorLine/cursorColumn/scrollTop/deleted を追加。collectAppState の
  実値化・active_tab 往復復元・削除済みタブ回復導線・safeEmpty 通知・goto を paths[0] へ・confirmAll の Promise.all 化・
  persistAppState デバウンス＋beforeunload 即時 flush・switchFolder（未保存確認）・openPath/openNewFileTab（種別分岐）・
  noteRecent 呼び出し。

### docs（系統C 所見台帳・書込可）
- `docs/acceptance-findings.md`（拡張）— T-008 に iteration2 節を追加（content_hash 実体化/active_tab 往復/削除済みタブ
  回復導線/巨大ファイルガード/最近使った項目+ジャンプリスト/フォルダ切替/存在しないパス・H8〜H12 を系統C で追加）。

## feedback_structured 対応状況

### high（4 件）すべて対応
1. **状態復元の実体不全（cursor/scroll/content_hash ダミー固定）** → 解消。EditorHandle に getCursor/getScrollTop を実装し、
   開く/保存/外部リロード/巻き戻し時に backend `hash_content`（`pika-core::hashing::hash_normalized_lf`＝probe_path と
   同一規則）でタブの content_hash を実値で詰める。collectAppState はカーソル/スクロールを実値収集。これで復元3分岐
   **別物=未読復元** が production で発火する（従来は content_hash 空で probe が常に Same を返す死に枝だった）。
2. **active_tab 往復欠落** → 解消。`active_tab_path()` で index→パス解決し `RestoreOutcomeDto.active_path` に載せ、復元後に
   **パスで** 再アクティブ化（復元順に依らない）。`gotoPosition`/`scrollToLine` で位置も戻す。
3. **削除済みタブの回復導線欠落** → 解消。起動復元で外部削除されていたタブを取消線＋× の削除済みタブとして残し、空エディタを
   出して「確認済み時点に戻す（rollback）」へ到達可能にした（退避/ベースラインは snapshot に残る）。rollback/保存で
   削除フラグを解除する。
4. **起動復元の同期 FS I/O ホットパス（巨大ファイルガード/並列化）** → 巨大ファイルガードを実装。`probe_path` を
   metadata len 先読みにして **10MB 以上は全量読込せず Same 扱い**（spec「10MB 以上はハッシュのみ」と整合・起動ホット
   パスを全量読込でブロックしない）。**独立タブ probe の並列化は見送り**：size guard で per-タブ最大コストが上限付き
   （<10MB 1 回読込）になり、復元タブ数は通常数本なのでスレッド orchestration の追加複雑性は YAGNI と判断（report に記載＝
   評価へ委ねる）。並列化が必要になるのは「10MB 未満ファイルを多数タブで同時復元し起動 0.5 秒に近づく」実測が出た段で、
   それは sprint6 の CM6 実測（系統C）で検知する設計。

### medium（5 件）対応
- **破損空起動の可視化** → restoreOnStartup で safe_empty 時に「前回状態を読めず空起動・元の設定は保全し上書きしない」旨を
  通知バー（warn）で提示。
- **複数パス + -g 併用の goto 適用先** → onOpenRequestEvent を openPath ベースにし、goto は **paths[0]** を再アクティブ化して
  から適用（ループ末尾タブへ飛ばない）。
- **確認一括の N+1 IPC** → onConfirmAll の差分先読みを `Promise.all` で並行化（直列線形劣化を解消）。
- **state.json 保存頻度のデバウンス** → persistAppState を 400ms デバウンス（連続オープンで書込を合体）。終了時
  （beforeunload）は persistAppStateNow で即時 flush（データ喪失リスクなし）。
- **ウィンドウ前面化の責務** → 系統C 検証項目として acceptance-findings T-008 に責務分担を明記（前面化は backend
  `bring_to_front`・フロントは open-request で開くのみ）。

### low（6 件）対応/記録
- **アトミック書込の fsync 省略** → state_store::save に `sync_all` を追加（rename 前にディスクへ落とす）。
- **固定 tmp 名の競合余地** → tmp 名に PID サフィックスを付与（同居プロセスの相互上書きを構造的に排除）。
- traversal 最終封じ込めの責務文書化 / エラーメッセージのパス・SID 平文 / named pipe ReadFile タイムアウト無し /
  転送無言失敗時の診断手掛かり → いずれも sprint4 プレビュー・sprint7 診断ログ（要件12.3）への将来引継ぎ事項。本スプリント
  範囲では実害が薄く（同一ユーザー権限境界内・Result 返却のみで無条件ログ出力経路なし）、構造変更は当該スプリントで一本化
  するのが適切。eval の low 記録を踏襲し本ターンでは変更しない（評価へ委ねる）。

## should criteria 実装状況（iteration1 は 0/3 → 本ターン 3/3）

1. **最近使った項目＋ジャンプリスト（要件10.2）**: 実装。`pika-core::recent::RecentList`（LRU・重複排除・各20件上限・
   cargo test 6 件）で state.json 保持。`note_recent` command が read-modify-write（未知/破損は保全）で更新し、
   `jumplist.rs` の `SHAddToRecentDocs(SHARD_PATHW)` で OS の Recent ジャンプリストへ登録（実描画は系統C H12）。
   ICustomDestinationList のカスタムカテゴリは要件外（足さない）。
2. **フォルダ切替（要件3.2）**: 実装。switchFolder が起動中の別フォルダ指定で未保存タブの破棄確認を挟み、前フォルダの
   タブ/未読/エディタを畳んで切替える（複数フォルダ同時オープンはしない＝要件14章）。フォルダ外ファイルの単体監視は
   既存 onOpenRequest 経路（タブで開く）に乗る。
3. **存在しないファイルパス=新規タブ/存在しないフォルダ=エラー（要件3.2）**: 実装。`path_kind` command で種別判定し、
   missing は openNewFileTab（保存で実体を作る空タブ）、dir は switchFolder、存在しないフォルダは openWorkspace の
   is_dir 検査でエラー通知。

## must criteria 実装状況（iteration1 で全充足・本ターンで実体化を強化）

iteration1 で must 7 件は全充足（eval d_accept=100）。本ターンは must「state.json 復元3分岐（カーソル/スクロール保存）」の
**実体不全を解消**（high#1）し、「単一インスタンス転送の -g カーソル」を paths[0] に正した（medium）。must の決定論ロジックは
pika-core（cli/ipc/path_verify/state/hashing/recent）に集約済み・cargo test で観測。系統C（H1〜H12）は Windows 実機。

## テスト化できなかった criteria とその理由

- **ジャンプリスト実描画（H12）/単一インスタンス実機（H1/H2/H4）/state.json 復元の GUI 反映（H8〜H11）**: GUI/タスクバー/
  既存プロセスへの転送・前面化は Windows 実機が要るため cargo test 不能。系統C（acceptance-findings T-008）へ記録。
- **SHAddToRecentDocs / named pipe / spawn / アトミック書込の OS 呼び出し**: FFI 実行は単体テスト不能。純粋ロジック
  （recent の LRU/重複/上限・hashing・state 復元3分岐・active_tab_path 解決・パス再検証）を pika-core へ切り出し全て
  cargo test で観測。OS 呼び出し層は cargo build（警告エラー扱い）＋型チェックで最低保証（spec.md 系統A補）。
- **probe_path の巨大ファイルガード**: hashing 側の `HUGE_FILE_THRESHOLD_BYTES`(=10MB) を cargo test で観測。FS metadata
  len 分岐自体は I/O を伴うため state_store（src-tauri・型/コンパイルゲート）に置いた。

## 自己実行した verify の結果（sprint verify: cargo test / cargo build）

- `cargo test`: PASS。pika-core **207 件**（iteration1 比 +14＝hashing 6・recent 6・state +2）／pika-cli 11 件／
  pika-app(bin) 5 件。全 0 failed。
- `cargo build`（crates＋src-tauri・workspace lints warnings=deny）: Finished・exit 0（警告ゼロ）。
- `npm run typecheck`（tsc -p tsconfig.app.json）: exit 0（エラーゼロ）。
- `cargo fmt --check`: exit 0（整形クリーン）。
- 補足: `cargo audit` は本環境に未インストール（sprint 5 の verify 対象外。sprint 7 で must 化予定）。

合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

DONE: C:/dev/pika_editor/dev/turns/turn-5-2-generator.md
