# ハンドオフ: エンコーディングを指定して開き直す（TF3 / 要件5.2・5.6）

> **状態**: ✅ 完了（2026-06-25・ブランチ `feat/reopen-encoding`・実装コミット `1d72bb5`）。PR#1 マージ後の main から分岐して実装。
> 3層（pika-core `decode_with`／src-tauri `reopen_document_with_encoding`／frontend 表示メニュー結線+dirty確認）を実装。
> `cargo test`(pika-core 401 PASS・decode_with 5件追加)／`cargo build`(warnings=deny)／`npm run typecheck` 緑。
> 系統C 実機GUI（PrintWindow検証）で TF3/C2 を確認済み（acceptance.md 更新済み）。
> **背景**: 日本語 BOM なし UTF-16 の無言データ喪失修正（コミット `fdd70f7`）は「無言喪失→警告表示」までで、
> 警告が **actionable でない**（実際に別エンコーディングで開き直す手段が未結線）。本作業でそのギャップを閉じた。

## ゴール

要件5.6 受け入れ基準「Shift_JIS と誤判定された UTF-8 ファイルを『エンコーディングを指定して開き直す』で
正しく再表示できる」＝ acceptance.md **TF3** を満たす。あわせて日本語 BOM なし UTF-16（`fdd70f7` で警告のみ
立てたケース）を UTF-16LE/BE で開き直して正しく表示できるようにする。

「表示」メニューの「エンコーディング」サブメニュー（`src/main.ts` 1435 行付近・**現状は表示専用**）を実結線する。

## 実装（3層の縦切り）

### 1. pika-core: 指定エンコーディングで強制デコードする公開 API

ファイル: `crates/pika-core/src/encoding.rs`

現状 `pub fn decode(bytes) -> DecodedFile` は自動判定のみ。内部の `decode_strict(enc, bytes)` を使い、
**判定をスキップして指定エンコーディングで開く**公開関数を追加する:

```rust
/// 指定エンコーディングで強制的にデコードする（自動判定を行わない・要件5.6 Reopen）。
/// 失敗（そのエンコーディングで妥当でない）時は None を返し、呼び出し側がユーザーへ通知する。
pub fn decode_with(encoding: TextEncoding, bytes: &[u8]) -> Option<DecodedFile>
```

- BOM の扱い: 指定が Utf16Le/Be/Utf8 で先頭に対応 BOM があれば剥がして `has_bom: true`、無ければ `has_bom: false`。
  保存時に往復させるため `DecodedFile.has_bom` を正しく立てる（`encode_for_save` が BOM 維持を見る）。
- `line_ending` は `classify_line_ending` で分類（既存）。
- `had_decode_warning` は **false**（ユーザーが明示選択したため）。
- そのエンコーディングで `decode_strict` が失敗（例: 不正な UTF-16 シーケンス）なら `None`。
- テスト: 既存 `decode` のラウンドトリップ資産に加え、(a) BOM なし日本語 UTF-16LE を `decode_with(Utf16Le, ..)`
  で `"あいうえお"` に復元、(b) Shift_JIS バイトを `decode_with(ShiftJis, ..)` で復元、(c) UTF-8 を `decode_with(Utf16Be,..)`
  に通して `None`（妥当でない）になるケース。

### 2. src-tauri: 再オープン用コマンド

ファイル: `src-tauri/src/document.rs`（`open_document` の隣）、登録は `src-tauri/src/main.rs` の `generate_handler!`

```rust
#[tauri::command]
pub fn reopen_document_with_encoding(
    path: String,
    encoding: String,                 // "utf-8" | "utf-16le" | "utf-16be" | "shift_jis"（enc_to_dto/enc_from_dto と対応）
    access: State<'_, crate::access::AccessControl>,
) -> Result<OpenedDocument, String>
```

- 先頭で `access.verify_read(&path)` 封じ込め（`open_document` と同作法）。
- 段階制は `open_document` のロジックを再利用（`FileStage::from_size`）。**normal/stage1 のみ**再オープン対象
  （stage2=仮想ビューア/too-large は編集テキストを持たないので非対象。フロントはメニュー項目を無効化）。
- バイトを読み、`decode_with(enc, &bytes)` を呼ぶ。`None` なら `Err("選択したエンコーディングでは読めません: ...")`。
- 返す `OpenedDocument` は `encoding` を選択値、`decode_warning: false` にする。他フィールドは `open_document` と同形。
- enc 文字列⇔`TextEncoding` 変換は既存 `enc_to_dto`/（必要なら `enc_from_dto` を追加）。

### 3. frontend: 表示メニュー結線 ＋ タブ差し替え

ファイル: `src/main.ts`（エンコーディングサブメニュー 1435 行付近、`openDocument` 呼び出し 446 行付近、
保存が参照する `tab.encoding`）、`src/<invoke ラッパ>`（`openDocument` の隣に `reopenDocumentWithEncoding` を追加）

- サブメニューの各項目（UTF-8 / UTF-16 LE / UTF-16 BE / Shift_JIS）に、選択で `reopen_document_with_encoding`
  を呼ぶハンドラを結線。現在のエンコーディングには ✓（既存の現在値表示ロジックを流用）。
- **dirty タブの保護（最優先原則1）**: 未保存編集があるタブを再オープンするとバッファを差し替えるため、
  破棄確認ダイアログ（［破棄して開き直す/キャンセル］）を出す。キャンセルなら何もしない。
- 再オープン成功後: エディタ内容を差し替え、`tab.encoding`/`has_bom`/`line_ending` を新値で更新
  （**以後の保存 `save_document` がこのエンコーディングを維持する**＝要件5.2）。content_hash/baseline は
  開いた内容で再評価。メニューを再描画（build 再評価）。
- 失敗時（`Err`）はトーストで通知し、現状を変えない。
- 任意の追加導線: `decode_warning` トースト（main.ts:474）から直接エンコーディング選択へ誘導する近道を足すと UX 向上
  （TF3 必須は「表示」メニュー経路のみ）。

## 受け入れ（系統C・手動 acceptance）

- **TF3**: Shift_JIS と誤判定された UTF-8 を「表示→エンコーディング→UTF-8」で開き直して正しく再表示。
- **UTF-16 追補**: BOM なし日本語 UTF-16LE ファイル（`fdd70f7` で警告が出るもの）を「UTF-16 LE」で開き直すと
  `あいうえお` 等が正しく表示される。
- dirty タブで再オープンすると破棄確認が出て、キャンセルで編集が保持される。
- 再オープン後に保存すると、選択したエンコーディングと改行コードが維持される（TF2 と同じ往復不変）。

## 検証（完了主張前）

- `cargo test`（pika-core の `decode_with` ラウンドトリップ／強制失敗ケース）
- `cargo build`（全ワークスペース・`warnings = "deny"`）
- `npm run typecheck`
- GUI 実機: 上記 acceptance を `docs/acceptance.md` TF3/TF2 へ反映（系統C）

## 触るファイル一覧

- `crates/pika-core/src/encoding.rs`（`decode_with` ＋テスト）
- `src-tauri/src/document.rs`（`reopen_document_with_encoding`）、`src-tauri/src/main.rs`（コマンド登録）
- `src/main.ts`（メニュー結線・dirty 確認・タブ差し替え）、frontend の invoke ラッパ
- `docs/acceptance.md`（TF3 結果更新）
