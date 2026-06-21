# turn-3-1 generator report — sprint 3（スナップショット/差分/確認済み・中心体験③④貫通）

- sprint_id: 3 / iteration: 1 / feedback: null（初回ターン）
- 目標（再注入）: 中心体験③④「差分→確認済み」を Tauri 上で貫通させる。pika-core に **snapshot**
  （ベースライン・退避・content-addressed object・容量GC・自己記述メタ）と **diff**（similar Myers＋
  文字単位フォールバック・LF 正規化照合）を実装し `cargo test` で固める。read-only unified 差分レンダラを
  frontend に配線し、確認済み/すべて確認済み/巻き戻し（**退避が最後の砦**）を結線して
  「開く→外部変更→差分→確認済み」の縦切りを通す。設計の核＝**ロジックは UI 非依存の pika-core に集約し
  決定論ゲートに乗せ、command/frontend は薄い境界**。最上位原則「データを失わない」を守る。

## 自己実行した verify の結果

| verify | コマンド | 結果 |
|---|---|---|
| 系統A（決定論ゲート・sprint verify） | `cargo test` | **PASS（exit 0）**。pika-core **92 件**（うち sprint3 新規 **50 件**＝diff 15・snapshot 25・review 10）・pika-cli 4 件・doctest 0 |
| 系統A補（コンパイル/型・sprint verify） | `cargo build`（crates＋src-tauri・`warnings="deny"`） | **PASS（exit 0・警告ゼロ）** |
| 系統A補（frontend 型） | `npm run typecheck`（`tsc -p tsconfig.app.json`・strict） | **PASS（exit 0）** |
| 参考 | `npm run build`（tsc＋vite build）／`cargo fmt --check` | PASS（dist/ 生成・整形差分なし） |

注: 合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

## 変更ファイル一覧

### 新規（pika-core・cargo test 決定論ゲート対象）
- `crates/pika-core/src/diff/mod.rs`（diff モジュール根・re-export）
- `crates/pika-core/src/diff/line.rs`（LF 正規化・行 LCS〔similar Myers〕・空ファイル/新規/全削除/累積差分・置換ブロックの行内セグメント付与。9 テスト）
- `crates/pika-core/src/diff/inline.rs`（語/grapheme フォールバック判定〔自前部分〕＋ similar による行内 LCS・結合文字保護。6 テスト）
- `crates/pika-core/src/snapshot/mod.rs`（snapshot モジュール根・re-export）
- `crates/pika-core/src/snapshot/object.rs`（content-addressed の LF 正規化ハッシュ・zstd 往復・退避4種・自己記述メタ。5 テスト）
- `crates/pika-core/src/snapshot/store.rs`（ベースライン常に1件・退避 LRU・参照計数・**index 破損→object メタから退避一覧再生成**。8 テスト）
- `crates/pika-core/src/snapshot/gc.rs`（容量管理＝500MB／14日保護／90日 stale・保護のみ超過は削除せず超過バイト返却。6 テスト）
- `crates/pika-core/src/snapshot/policy.rs`（機密/画像/10MB境界＝ハッシュのみ判定。ちょうど10MB含む。6 テスト）
- `crates/pika-core/src/review.rs`（確認済み/すべて確認済み/巻き戻しの判定・mtime/ハッシュ再照合・退避不能ガード・退避を握り潰さない。10 テスト）

### 新規（src-tauri 配線層・薄い境界）
- `src-tauri/src/snapshot.rs`（`compute_file_diff`/`confirm_file`/`confirm_all`/`rollback_file` command・ベースライン内容 object 保持・DTO 化。ロジックは全て pika-core 委譲）

### 変更
- `crates/pika-core/src/lib.rs`（`diff`/`snapshot`/`review` モジュール追加）
- `Cargo.toml`（workspace deps に `similar="2"`・`unicode-segmentation="1"`・`zstd="0.13"`。design doc 2章の指定スタック）
- `crates/pika-core/Cargo.toml`（similar/unicode-segmentation/twox-hash/zstd を pika-core へ。**tauri/wry は無し＝コアは UI を知らない を維持**）
- `src-tauri/src/main.rs`（`mod snapshot;`・`SnapshotService` を managed state 登録・4 command を handler 追加）
- `src-tauri/src/commands.rs`（`open_workspace` で全既読スタートのベースライン内容取得＝要件8.1）
- `src/ipc.ts`（`DiffLine`/`FileDiff`/`DiffSegment` 型・`computeFileDiff`/`confirmFile`/`confirmAll`/`rollbackFile` 追加）
- `src/diff/index.ts`（**read-only unified 差分レンダラ**＝行頭±記号・変更語下線/太字・色非依存・F8/Shift+F8 ブロックジャンプ・差分非対象/変更なし表示）
- `src/main.ts`（差分トグル Ctrl+\・Ctrl+E でソース復帰・確認済み/すべて確認済み/巻き戻しボタン配線・タブ切替時の差分再描画）
- `src/ui/unread.ts`（`confirmTargets()`＝「すべて確認済み」用の未読パス一覧。削除済みは対象外）
- `src/index.html`（差分/確認済みにする/すべて確認済み/確認済み時点に戻すボタン・差分ホスト #diff-host）
- `src/styles/app.css`（read-only 差分の配色〔既存 diff トークン流用〕・変更語の下線/太字・現在ブロック枠・差分面占有切替）
- `docs/acceptance.md`（**系統C** TD1〜TD9 を節番号併記で追加）
- `docs/acceptance-findings.md`（**系統C** T-006 スナップショット/差分/確認済み を追記）

## must criteria ごとの実装状況

1. **diff（pika-core）** — 充足。`diff/line.rs` が similar(Myers) 行差分＋LF 正規化照合（`改行のみの差は差分に出さない`）・
   空ファイル（`空ファイル同士は差分なし`）・新規=全行追加（`新規はベースラインなしで全行追加`）・累積差分
   （`累積差分は前回確認時点から現在まで`）を観測。`diff/inline.rs` が **語境界が取れる行か否かの判定**（`pick_granularity`）で
   英文=語単位（`英文は語境界が取れるので語単位`）・日本語=grapheme 単位（`日本語は語境界が取れないので_grapheme_単位へフォールバック`）・
   片側でも語境界無しは安全側 grapheme（`片側でも語境界が取れなければ_grapheme_に倒す`）・結合文字保護
   （`結合文字を_grapheme_境界で壊さない`）を観測。LCS 自体は similar に委ね、自前は語境界判定のみ（design doc 7章）。
2. **snapshot（pika-core）** — 充足。`object.rs` が LF 正規化 content-addressed ハッシュ（`改行のみ違う内容は同じハッシュ`）＋
   zstd 往復（`zstd_は往復で元に戻る`/`壊れた_zstd_は_none`）＋退避4種の自己記述メタ。`store.rs` がベースライン常に1件
   （`ベースラインは常に1件で上書き更新`）・**index 破損→object メタから退避一覧再生成**（`index_破損時に_object_メタから退避一覧を再生成できる`・
   `実体が欠けた_object_メタは復元しない`）を観測。
3. **容量管理（pika-core）** — 充足。`store.rs` がファイルごと最新10件 LRU（`退避はファイルごと最新10件_lru`・
   `未復元退避は_lru_で復元済みより後に押し出す`）・共有 object 全参照確認後削除（`共有_object_は全参照不在を確認後に物理削除可能`・
   `is_object_referenced`/`live_objects`）・baseline-replace は10件枠と別（`baseline_replace_は10件枠と別で押し出さない`）を観測。
   `gc.rs` が500MB＋**14日保護**（`未復元かつ14日以内の退避は容量gc保護`／保護のみ超過は削除せず超過バイト返却）・
   90日 stale（`_90日経過ワークスペースは_stale`）を観測。
4. **機密/10MB境界（pika-core）** — 充足。`policy.rs` が機密（`機密ファイルはハッシュのみ`）・画像（`画像はハッシュのみ`）・
   **ちょうど10MBもハッシュのみ**（`ちょうど_10mb_はハッシュのみ`）・10MB未満のみ内容保存（`_10mb_未満のテキストは内容保存`）を観測。
5. **確認済みフロー結線** — 充足（判定は cargo test 済み・command/frontend は配線）。`review.rs::decide_confirm` が
   **確定直前の mtime/ハッシュ再照合**で変化検知→中断（`確定直前にディスクが変化していたら中断して再差分`・`mtime_だけ変化でも中断する`）。
   `decide_confirm_all` が**実行開始時点フリーズ＋変化ファイルをスキップ（未読維持）＋更新前を baseline-replace 退避**
   （`すべて確認済みは変化ファイルをスキップし他は更新`）を観測。src-tauri `confirm_file`/`confirm_all`/`rollback_file` で結線し、
   frontend は未読解除・ツリー/タブのマーク解除を配線。
6. **退避結合の Result を握り潰さない（最上位原則）** — 充足。`review.rs::ReviewError`（StashFailed/StashImpossibleBlocked）を
   定義し、巻き戻しは退避不能で必ずエラー（`ベースラインがハッシュのみなら巻き戻しブロック`・`現在内容が退避不能なら巻き戻しブロック`）。
   src-tauri 側も退避（object 保存）が成立してからベースラインを進める構造（confirm_all は更新前 object を退避してから set_baseline）。
   退避不能ガード `guard_destructive` が既定ブロック・許可フラグで通す（`退避不能ガードは既定ブロックし許可フラグで通す`）。
7. **read-only unified 差分レンダラ（frontend）** — 充足（配線・実描画は系統C）。`src/diff/index.ts` が Rust 算出 hunk を DOM 描画し
   行頭 +/- 記号（`TAG_MARK`）・変更語/grapheme の下線/太字（`.diff-seg-changed`＝色非依存）・前後変更ジャンプ F8/Shift+F8
   （`groupBlocks`/`focusBlock`）を実装。差分面は `aria-readonly`／編集は Ctrl+E でソースへ（main.ts）。
8. **cargo test PASS／cargo build／frontend 型チェック exit 0** — 充足（上表）。

## should criteria の実装状況

- **退避不能ガード（要件7.3）** — 実装。`review.rs::guard_destructive`／`decide_rollback` が10MB以上・画像への破壊的操作を
  既定ブロック・設定で許可（cargo test 観測）。src-tauri `rollback_file` が `decide_rollback` で判定。
- **差分は Rust 側で計算し UI を 200ms 超ブロックしない** — 配線。差分計算は backend command（Rust）で実行し frontend は DOM 描画のみ。
  サイズ/編集距離ガードの本格化は巨大ファイル sprint（6）の段階制と統合する旨を T-006 に記録。
- **ファイル単位の巻き戻し・フォルダ一括確認の起動配線** — 実装。巻き戻し（rollback 退避→ベースライン内容で reloadExternal）と
  すべて確認済み（フォルダ右クリック起動は sprint 7 のツリー右クリック実装と統合・本スプリントはツールバーボタンで起動）。

## テスト化できなかった criteria とその理由（系統C／後続）

- **差分実描画・確認済み/巻き戻し実操作・実 FS のベースライン/退避・退避不能ガード実挙動**（must 7 の描画側・should の実機側）:
  Tauri/WebView2 実描画・実 FS が要るため cargo test では固められず系統C（acceptance.md TD1〜TD9・findings T-006）へ。
  **決定論ロジック（diff/snapshot/gc/policy/review）は 50 件の cargo test で全て観測**しており、系統C は実描画・実操作の確認に限定。
- **TD8（index 破損復元）・TD9（容量GC）の実機検証**: 本スプリントはベースライン内容 object を **メモリ保持で結線**
  （中心体験貫通を優先）。データルート配下への zstd 永続化＋windows crate 厳格 DACL は後続で同じ pika-core 判定
  （policy/gc/store）を再利用して実装する（design doc 11章）。**決定論ロジックは既に cargo test 済み**で、後続は永続化の結線のみ。
  この切り分け根拠を T-006 に明記した。

## 設計判断メモ（ドリフト追跡）

- **ロジックは pika-core に集約・command/frontend は薄い境界**。差分計算・退避方針・容量管理・確認済み/巻き戻し判定は
  すべて pika-core（UI/Tauri/wry/FS 非依存）に置き 50 件の cargo test で固めた。src-tauri は FS 読取＋pika-core 呼び出し＋
  DTO 化のみ。`cargo tree -p pika-core` に tauri/wry は乗らない（「コアは UI を知らない」を構造で担保）。
- **依存追加の妥当性**: similar/unicode-segmentation/zstd はいずれも design doc 2章（技術スタック表）が指定する承認済み crate。
  UI 系依存ではなく原則違反なし。`unicode-segmentation` は既に lockfile に存在（transitive）。
- **LF 正規化の一元化**: 差分照合・content-addressed ハッシュ・自己保存抑制（sprint2）を**同一の LF 正規化規則**で揃え、
  改行のみの差を未読/差分/object 重複に出さない（要件8.1）。`diff::normalize_lf` を object 内容の正規化にも再利用。
- **パスキー正規化**: フロントの未読ストアは fs-changed パスを `/` 区切りへ正規化、open_workspace はネイティブ `\`。
  SnapshotService の索引アクセスを `normalize_path_key`（`\`→`/`）で揃え、両者を同一キーで引けるようにした
  （FS 読取は元パスをそのまま使う＝区切り混在に強い）。
- **本スプリントの object はメモリ保持**: 中心体験③④の貫通を最優先し、ベースライン内容 object はメモリ保持で結線した。
  永続化（zstd 書込・DACL）は決定論ロジック（policy/gc/store）を一切変えず後続スプリントで結線できる構造
  （永続化側が pika-core の判定を呼ぶだけ）。spec「sprint 3 = 中心体験貫通」「実機・実測は系統C」と整合。
- **ベースライン更新の責務境界（sprint2 からの継承）**: 監視 drain/F5/オーバーフロー再同期は未読の算定のみで baseline 据え置き
  （取りこぼし防止）。内容変更/新規のベースライン確定は本スプリントの**確認済み操作**（confirm/confirm_all）で初めて前進する。
  sprint2 で「確認済みフロー実装（sprint 3）でここを結線する」と記したとおり結線した。

DONE: C:/dev/pika_editor/dev/turns/turn-3-1-generator.md
