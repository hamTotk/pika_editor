# turn 6-1 generator report — sprint 6（中心体験④『確認済みにする』と保存・衝突退避）iteration 1

## 概要

sprint 6 は「確認済み/すべて確認済み/巻き戻し（core/document ReviewFlow × core/snapshot
SnapshotStore）」を DocumentController から呼ぶフローと、保存（衝突検知→incoming退避→アトミック
置換）、フォーカス別ショートカット割当表を実装する。検証は spec.md の二系統どおり
(A) wx 非依存ロジックを `ctest --preset x64-core-test`（gtest）で決定論検証＝must、
(B) wx 依存の GUI 配線を `cmake --build --preset x64-release`（/W4・/WX）のコンパイル＋リンク成立＝must。
両 verify はローカルで exit 0。退避結合の `Result<T>` を握り潰さないこと（『データを失わない』最上位
原則の UI 側ガード／前回 report 持ち越し #5）を controller の must テストの中心に据えた。

## 変更ファイル一覧

### 新規（controller・pika_core に含め gtest 対象。系統A）

- `src/controller/document_controller.h` / `.cpp`
  - `DocumentController`：1 ワークスペース分の `SnapshotStore` を束ね、その上に `ReviewFlow` を載せる
    オーケストレータ。`confirm` / `confirm_all` / `revert_all` / `rollback` / `prepare_save` を提供。
    index は SnapshotStore/ReviewFlow と同一規約で参照で受け取り破壊的更新、未読集合（`UnreadSet&`）
    への反映までを担う（index.json 永続化は呼び出し側）。
  - `confirm`：`ReviewFlow::confirm` の Result を握り潰さず、**成功時のみ**未読を除去。失敗（退避不能・
    put I/O 障害）は未読を維持してエラーを返す（ベースライン半端変異なし）。
  - `confirm_all`：core の `AllConfirmedResult`（confirmed / skipped）をそのまま引き継ぎ、confirmed のみ
    未読除去・skipped は未読維持（退避失敗/並行変化スキップを握り潰さない）。
  - `revert_all`：baseline-replace バッチ取消で confirmed を未読へ戻す（旧内容へ戻る＝再び未読）。
  - `rollback`：`ReviewFlow::rollback` に委譲（退避が先・退避不能は Unsupported を握り潰さず返す）。
  - `prepare_save`：design 5.3 の保存前チェックを純ロジック化。(1) 表現可能性（`can_encode`）→
    BlockedEncoding、(2) 現ディスク内容ハッシュ再計算で衝突検知（キャッシュ値不使用・F5）、
    (3) 衝突時は incoming 退避→退避不能=BlockedUnstashable・退避 I/O 失敗=BlockedStashFailed・
    成功=Proceed＋退避 object 名。いずれの Blocked* でもディスク・ベースラインを変更しない。
- `src/controller/shortcut_table.h` / `.cpp`
  - `dispatch_shortcut(FocusContext, KeyChord) → ShortcutAction`：design 10章 J3 のフォーカス別
    ディスパッチを単一決定論テーブル化。Ctrl+Alt+Enter=ConfirmAll（フォーカス非依存・最優先）、
    Ctrl+Enter（差分/プレビュー）=Confirm、Ctrl+Shift+Enter（エディタ）=Confirm（Ctrl+Enter は改行に
    譲る）、それ以外=None（既定動作を奪わない）。

### 新規（テスト。系統A）

- `tests/controller/document_controller_test.cpp`（13 件）：確認済みのベースライン更新＋未読解除、
  退避失敗時の未読維持＋エラー伝播（objects 位置をファイル化して put 失敗注入）、巨大/画像のハッシュ
  ベースラインのみ更新、confirm_all の confirmed/skipped 弁別と未読集合反映、退避失敗での skip＋未読
  維持＋ベースライン不変、revert_all の旧内容復元＋未読再マーク、rollback の退避＋ベースライン返却、
  退避不能 rollback=Unsupported、保存の衝突なし Proceed、衝突→incoming 退避（復元一致）、衝突＋退避
  不能=BlockedUnstashable、表現不能（CP932 絵文字）=BlockedEncoding、退避 I/O 失敗=BlockedStashFailed。
- `tests/controller/shortcut_table_test.cpp`（9 件）：J3 の全分岐を観測（差分/プレビュー Ctrl+Enter、
  エディタ Ctrl+Enter 無割当・Ctrl+Shift+Enter 確認、Ctrl+Alt+Enter 全フォーカス一括、ツリー無割当、
  修飾なし Enter 無割当、Ctrl のみ無割当、Alt 優先）。

### 変更（GUI 配線。系統B＝コンパイル＋リンク成立で検証）

- `src/controller/workspace_controller.h`：`unread_mut()`（可変参照）を追加。DocumentController と
  同一未読集合を共有し二重管理を避ける（確認成功で除去・一括取消で戻す反映先）。
- `src/ui/main_frame.h` / `.cpp`：
  - コンストラクタに `data_root`（退避・ベースライン保存先）を追加。空なら退避フロー非活性
    （退避先未確定で破壊的操作を始めない。設計原則1）。
  - レビューメニュー（確認済み/すべて確認済み/巻き戻し）＋ファイルメニュー保存（Ctrl+S）を追加し、
    `on_confirm` / `on_confirm_all` / `on_rollback` / `on_save` を結線。各ハンドラは index を
    load→DocumentController 呼び出し→save し、Blocked*/エラーは `wxMessageBox` の通知へ変換
    （Result を握り潰さない）。
  - `on_char_hook`：`wxEVT_CHAR_HOOK` を 1 か所に集約し、`current_focus()`＋`KeyChord` から
    `dispatch_shortcut` で確認/一括確認へ振り分け（割当ありなら既定動作を消費＝改行を奪わない）。
  - `open_file`：保存衝突判定の素材（読み込み時点ハッシュ・エンコーディング・BOM）を `doc_meta_` に記録。
  - `active_content_class()`：機密（`is_sensitive_default`）・10MB 以上（`kContentSizeLimit`）で退避可否を判定。
- `src/ui/ui_messages.h` / `.cpp`：保存/確認/巻き戻しメニュー文言、衝突退避・表現不能・退避不能・退避
  失敗の通知文言を単一メッセージ定義へ追加（design 10章 K9。生文字列を散らさない）。
- `src/app/main_gui.cpp`：データルートを `resolve_data_root_path()`（`SHGetKnownFolderPath`＋
  `portable.txt` 検出を `controller::resolve_data_root` へ注入）で解決し MainFrame へ渡す。
- `src/CMakeLists.txt` / `tests/CMakeLists.txt`：新規 controller ソースとテストを登録。

## must criteria の実装状況（sprint 6）

| # | criteria | 状況 |
|---|---|---|
| 1 | DocumentController: confirm/confirm_all/rollback × SnapshotStore 結線・確認済みで未読除去・マーク解除 | 充足。`confirm` 成功で `UnreadSet` から除去（gtest）、GUI で `tabs_.set_unread(false)`＋`refresh_tree`。`confirm_all`/`rollback`/`revert_all` も結線・テスト済み |
| 2 | 退避結合の Result を握り潰さない（baseline-replace/incoming 退避失敗時はベースライン更新せず未読維持・エラー返却） | 充足。`confirm` の put 失敗で err＋未読維持（テスト）、`confirm_all` の退避失敗で skip＋未読維持＋ベースライン不変（テスト）、`prepare_save` の退避失敗で BlockedStashFailed（テスト） |
| 3 | 保存フロー: 現ディスク実内容ハッシュ再計算→衝突なら incoming 退避→上書き・表現不能は中断・退避不能は既定ブロック | 充足。`prepare_save` で Proceed/BlockedEncoding/BlockedUnstashable/BlockedStashFailed を gtest 観測。GUI は Proceed のみ `encode`→`write_atomic`（アトミック置換） |
| 4 | ショートカット割当表（Ctrl+Enter / Ctrl+Shift+Enter / Ctrl+Alt+Enter のフォーカス別ディスパッチ） | 充足。`dispatch_shortcut` を決定論テーブル化し全分岐 gtest 観測。GUI `on_char_hook` から到達 |
| 5 | `cmake --build --preset x64-release` 成功（/W4 exit 0）＋`ctest --preset x64-core-test` PASS | 充足。両 verify exit 0（下記） |

### must の自動テスト化状況・テスト化できなかった criteria

- must#1/#2/#3/#4 の判断ロジックはすべて wx 非依存純粋ロジックとして gtest 化（DocumentController 13 件・
  ShortcutTable 9 件）。退避失敗・衝突・表現不能・退避不能・並行変化スキップは決定論的なフォールト注入
  （objects 位置のファイル化・旧 object 物理削除・CP932 絵文字・freeze_hash 不一致）で観測している。
- **テスト化できなかった部分とその理由**：実 `wxMessageBox` 通知表示・`wxEVT_CHAR_HOOK` 実イベント配送・
  実 `FindFocus()` のフォーカス判定・Scintilla 実バッファからの保存・`write_atomic` の ReplaceFile 実挙動・
  メニュー実クリックは **wx/Win32 GUI 実機が必須**でユニットテスト不能。spec.md「検証戦略 系統B（コンパイル
  ＋リンク成立）＋系統C（手動・acceptance.md）」の設計どおり、判定可能部分を controller へ切り出して gtest
  化し、実配線はビルド成立＋手動（sprint8 で acceptance.md 化）へ寄せた。

## should criteria

| criteria | 状況 |
|---|---|
| ファイル単位の巻き戻し・フォルダ単位の一括確認をメニュー/右クリックから起動 | メニュー（レビュー＝確認済み/すべて確認済み/巻き戻し）から起動を配線。ツリー右クリックメニューは系統C（実描画必須）で sprint8 |
| 確認済み確定直前に mtime/ハッシュ再照合し不一致なら中断・再差分（E2） | confirm は確定直前に現ディスク内容を再読込して target.content にする（見ていない内容をベースライン化しない素材）。再照合での中断・再差分の自動ループは差分 UI 結線（系統C）に依存するため report 明示で繰延べ |
| 保存は一時ファイル→ReplaceFile のアトミック書き込み（属性/ACL維持） | `util::write_atomic`（ReplaceFile 経路・属性/ACL 維持・既存 sprint2 実装）で配線 |

## 自己実行した verify の結果（合否の正本は run-dev が別途実行）

- `ctest --preset x64-core-test` → **Passed**（pika_tests_build + pika_tests、100% / 0 failed）。
  gtest 全 **547 件 PASS**（前 sprint 525 → 本 sprint +22〔DocumentController 13・ShortcutTable 9、
  既存に +0 回帰なし〕で増加。非回帰）。exit 0。
- `cmake --build --preset x64-release` → **exit 0**（pika_core / pika_ui / pika.exe / pika.com /
  pika_tests を /W4・/WX でコンパイル＋リンク成功。警告ゼロ）。POST_BUILD の assets コピーも成功。

## 残課題（スコープ外・後続/系統C）

- 通知バー集約 ViewModel（最大3本＋他N件・優先順位）は sprint7。本 sprint の Blocked*/skip 通知は暫定
  `wxMessageBox`／ステータス文言で提示し、集約は sprint7 で置き換える（confirm_all の skipped 件数提示も）。
- ベースライン供給（`has_baseline`）に基づく差分トグル有効化は、確認済みで index にベースラインが入った後に
  効くが、差分トグル文脈への反映は差分 UI（sprint5 結線）と本 sprint の index を結ぶ追加配線が要るため
  系統C（実描画）で確認する。
- E2 の「確定直前 mtime/ハッシュ不一致で再差分」の自動再差分ループ・ツリー右クリック起動・巻き戻し後の
  バッファ再読込の実描画は系統C（acceptance.md・sprint8）。

DONE: C:/dev/pika_editor/dev/turns/turn-6-1-generator.md
