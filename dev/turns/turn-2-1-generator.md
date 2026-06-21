# turn-2-1 generator report — sprint 2（監視 watcher 自前合成層）

- sprint_id: 2 / iteration: 1 / feedback: null（初回ターン）
- 目標（再注入）: 中心体験②「外部変更を反映」の基盤＝**watcher 自前合成層**を pika-core に実装し
  `cargo test` で固める。notify の raw event を入力に、デバウンス/合体・rename 正規化（FileId 補強）・
  自己保存抑制（ハッシュ一致主条件・ワンショット）・オーバーフロー再同期を行い、監視スレッドから
  `emit('fs-changed')` でフロントの未読バッジを更新する。design doc 15章-2（notify オーバーフロー表面化）を
  系統C へ。設計の要：**合成ロジックは UI/notify 非依存の pika-core に集約し決定論ゲートに乗せる**。

## 自己実行した verify の結果

| verify | コマンド | 結果 |
|---|---|---|
| 系統A（決定論ゲート・sprint verify） | `cargo test` | **PASS（exit 0）**。pika-core 42 件（うち watcher 28 件 新規）・pika-cli 4 件・doctest 0 |
| 系統A補（コンパイル/型・sprint verify） | `cargo build`（crates＋src-tauri・`warnings="deny"`） | **PASS（exit 0・警告ゼロ）** |
| 系統A補（frontend 型） | `npm run typecheck`（`tsc -p tsconfig.app.json`・strict） | **PASS（exit 0）** |
| 参考 | `npm run build`（tsc＋vite build） / `cargo fmt --check` | PASS（dist/ 生成・整形差分なし） |

注: 合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

## 変更ファイル一覧

### 新規（pika-core 自前合成層・cargo test 決定論ゲート対象）
- `crates/pika-core/src/watcher/mod.rs`（モジュール根・re-export）
- `crates/pika-core/src/watcher/event.rs`（`RawFsEvent`/`FsChange`/`FileId` の抽象入出力型。notify 型を漏らさない）
- `crates/pika-core/src/watcher/debounce.rs`（デバウンス/合体・静穏期間＋mtime/サイズ安定での確定読み判定。8 テスト）
- `crates/pika-core/src/watcher/self_save.rs`（自己保存抑制＝ハッシュ一致主条件・ワンショット・時刻窓は補助安全弁。7 テスト）
- `crates/pika-core/src/watcher/rename.rs`（rename 旧名/新名ペア正規化＝FileId 補強・スワップ/往復/クロスディレクトリ/上書き/片側欠落。8 テスト）
- `crates/pika-core/src/watcher/overflow.rs`（オーバーフロー再同期＝全再列挙→mtime/サイズ→ハッシュ比較。ポーリング/F5 と共有。5 テスト）

### 新規（src-tauri 配線層・OS 監視と emit の橋渡し）
- `src-tauri/src/watcher.rs`（notify→`RawFsEvent` 変換・監視スレッド・合成→`emit('fs-changed')`・ポーリングフォールバック・F5・FSエッジ〔シンボリックリンク循環検出・OneDrive プレースホルダ除外〕）

### 変更
- `crates/pika-core/src/lib.rs`（`pub mod watcher;` 追加）
- `Cargo.toml`（workspace deps に `notify="6"`・`twox-hash="1"`。pika-core は依存しない＝src-tauri 専用）
- `src-tauri/Cargo.toml`（`notify`/`twox-hash` を src-tauri に追加）
- `src-tauri/src/main.rs`（`mod watcher;`・`WatcherService` を managed state 登録・`f5_resync` を handler 追加）
- `src-tauri/src/commands.rs`（`open_workspace` で監視開始・`save_file` で自己保存トークン登録＋ベースライン更新・`f5_resync` command 追加）
- `src/ipc.ts`（`FsChange`/`FsChangedPayload` 型・`f5Resync`/`onFsChanged`/`onWatchMode` 追加）
- `src/main.ts`（未読ストア配線・`fs-changed`/`watch-mode` 購読・クリーンタブ自動リロード・F5 キーバインド）
- `src/ui/unread.ts`（**新規**: 未読状態の単一源＝ツリー/タブ共有・フォルダ伝播カウント・rename 継承・確認済み解除）
- `src/ui/tree.ts`（未読マーク ± 変更 / ◆ 新規 / 取消線 削除・フォルダ伝播 淡 ± を描画）
- `src/ui/tabs.ts`（未読/未保存/削除済み重畳バッジ＝優先順位 削除済み ＞ 未保存 ＞ 未読・色非依存記号）
- `src/editor/index.ts`（`reloadExternal`＝外部リロードを単一トランザクション=1Undo境界・非dirty・カーソル/スクロール維持）
- `docs/acceptance.md`（**系統C** sprint 2 用 TC1〜TC6 を節番号併記で追加）
- `docs/acceptance-findings.md`（**系統C** T-005 watcher オーバーフロー表面化・rename FileId 補強の判断材料を追記）

## must criteria ごとの実装状況

1. **watcher 自前合成層（デバウンス/合体・確定読み）** — 充足。`debounce.rs` が同一パスの連続イベントを
   1 件に合体（`連続イベントが_1_件に合成される`）・静穏期間（既定100ms）経過かつ mtime/サイズが直近 2 回で
   安定するまで確定しない（`メタが安定するまで中途内容で確定しない`＝中途内容防止）を cargo test で観測。
   メタ取得不能 FS は時間のみで確定（縮退維持）。
2. **自己保存抑制（ハッシュ一致主条件・ワンショット・窓は補助）** — 充足。`self_save.rs` が
   ハッシュ一致で `Suppress`・ワンショット消費後の 2 回目は `External`・**窓超過でもハッシュ一致なら抑制**・
   **窓内でも内容相違なら外部変更**・保存後ハッシュ取得不能（`None`）は外部変更 を cargo test で観測。
3. **rename 正規化（FileId 補強・各ケース安全側）** — 充足。`rename.rs` が FileId 一致での単純 rename・
   相互スワップ A↔B（2 本の rename）・往復 A→B→A（NoChange へ畳む）・クロスディレクトリ移動・
   From 単独＝削除・To 単独＝新規・FileId 無しは時間窓内一意でペア化/窓超過は片側欠落へ を cargo test で観測。
   未読・ベースライン・退避の引き継ぎ対象を決定論で算定（`RenameResolution`）。
4. **オーバーフロー再同期（取りこぼし無し）** — 充足。`overflow.rs::resync_against_baseline` が
   新規/削除/変更を昇順・決定論で算定・**100 件同時新規を全件取りこぼさない**・mtime だけ動いて内容同一は
   未変更（LF 正規化照合と整合）・ハッシュ無しは安全側で変更扱い を cargo test で観測。
5. **監視スレッド配線＋emit** — 充足（配線・系統C で実描画）。`src-tauri/src/watcher.rs` の監視スレッドは
   notify raw event を `RawFsEvent` へ写して合成層へ供給し、定期 drain（50ms）で確定分を `emit('fs-changed',{changes})`。
   **監視スレッドは raw event をキューに積む→drain するだけ**で重い処理をせず UI を 200ms 超ブロックしない（design doc 3章）。
   フロントは `unread.ts` を単一源にツリー/タブの未読バッジ（±/◆/取消線・フォルダ伝播 淡 ±）を更新。
6. **ポーリングフォールバック＋F5** — 充足（配線・系統C で実動）。notify 購読失敗（ネットワーク/UNC）時は
   `spawn_polling_loop`（既定5秒）へ縮退し `watch-mode` で通知。`f5_resync` command＝F5 が同じ
   `resync_against_baseline` をオンデマンド実行（要件7.1/12.1）。フロントは F5 キーバインドで `f5Resync()` 呼び出し。
7. **cargo test PASS／cargo build／frontend 型チェック exit 0** — 充足（上表）。
8. **design doc 15章-2 watcher オーバーフロー表面化（系統C）** — 系統C へ記録。`acceptance-findings.md` T-005 に
   「notify が ERROR_NOTIFY_ENUM_DIR（Win32 0x3FF）を `Error`/`EventKind` で表面化するか実機検証。握り潰すなら
   windows crate `ReadDirectoryChangesW` 直叩きへ切替える」判断材料と、100 件同時の実機確認手順を記録。
   `is_overflow_error()` は `Generic("overflow")`／`Io(raw_os_error==0x3FF)` の双方を拾うフックを実装済み。

## should criteria の実装状況

- **外部変更反映（要件7.2・クリーンタブ自動リロード）** — 実装。`editor/index.ts::reloadExternal` が
  単一トランザクション（`ExternalReload` 注釈）=1Undo境界・非dirty・カーソル/スクロール近似維持で反映。
  `main.ts::autoReloadCleanTabs` が「現在タブがクリーンかつ modified 到来」時のみ自動リロード（未保存変更ありは通知のみ＝衝突は sprint 3）。**実描画は系統C（TC3）**。
- **FSエッジ（要件12.1）** — 実装（決定論部分）。`src-tauri/src/watcher.rs::walk` に
  シンボリックリンク循環検出（canonical 訪問済み集合）・OneDrive プレースホルダ除外（reparse/offline 属性で
  ベースライン取得対象外＝ダウンロード誘発回避）を実装。読み取り専用属性誘導は sprint 3/6 の保存層へ。**実機は系統C**。
- **監視/ポーリング/F5 実行中のステータス/通知表示** — 実装。`watch-mode` イベントを `notify()` で表示
  （ポーリング縮退・再同期中・オーバーフロー再同期中）。ステータスに未読件数を反映。

## テスト化できなかった criteria とその理由（系統C／ツール制約）

- **監視スレッド配線・emit 実描画・自動リロード実動・ポーリング/F5 実動・FSエッジ実挙動・notify オーバーフロー表面化**
  （must 5/6/8・should の実機側）: いずれも Tauri/WebView2 実機・実 FS・大量同時変更の再現が要るため
  `cargo test` で固められず系統C（`acceptance.md` TC1〜TC6・`acceptance-findings.md` T-005）へ。spec
  「検証戦略 系統C」「本フェーズの自動 verify に載せない（watcher オーバーフロー）」と整合。
  決定論で固められる**合成ロジック本体（デバウンス/自己保存抑制/rename/オーバーフロー再同期）は 28 件の
  cargo test で全て観測**しており、実機側は OS イベントと配線の確認に限定される。

## 設計判断メモ（ドリフト追跡）

- **合成ロジックは pika-core（notify 非依存）に集約**。`pika-core::watcher` は notify/Tauri/wry を一切知らず、
  `RawFsEvent` という抽象入力で受ける（`cargo tree -p pika-core` に notify は乗らない＝「コアは UI を知らない」を構造で担保）。
  notify 依存は src-tauri 側のみ（spec/CLAUDE.md の補助原則と整合）。
- **rename FileId 補強の制約**: `std::fs::Metadata` の `file_index()`/`volume_serial_number()` は安定版 Rust 未提供
  （nightly `windows_by_handle`）。`file_id_of()` は安定版で `None` を返し、rename ペア化は時間窓ベース（段2）に倒している。
  pika-core 側の正規化は FileId が来れば段1（FileId 主キー）で解決する設計済み（テストは FileId 有りで合格）。
  windows crate `GetFileInformationByHandle` 導入は T-005 の切替判断時（sprint 5/7）に検討する旨を findings に記録。
- **ハッシュは LF 正規化後に算出**（自己保存抑制・再同期照合とも）。改行のみの差を未読/差分に出さない方針（要件8.1）と整合。
  保存時は原文の改行を維持（照合用の正規化と保存内容は分離）。
- **ベースライン更新の責務境界**: 監視 drain では削除/rename のみベースラインを前進させ、内容変更/新規の
  ベースライン確定は確認済み操作（sprint 3）に委ねる（未読を消さないため）。F5/ポーリング/オーバーフロー再同期も
  未読の算定のみで baseline は据え置き（取りこぼし防止）。確認済みフロー実装（sprint 3）でここを結線する。
- **依存追加の妥当性**: notify は監視 raw event 供給の de-facto（design doc 2章/12章の指定）、twox-hash は
  重複/同一性判定用の高速ハッシュ（design doc 11章 twox-hash 方針）。いずれも UI 系依存ではなく原則違反なし。

DONE: C:/dev/pika_editor/dev/turns/turn-2-1-generator.md
