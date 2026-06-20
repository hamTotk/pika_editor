# turn-3-1 generator report — sprint 3（中心体験①『開く』の GUI/プラットフォーム配線）

## sprint goal（再注入）

main_gui.cpp（現スタブ）を実体化し MainFrame（メニュー/左ツリー/タブバー/通知バー/メイン/ステータス右下）を
構築、Scintilla エディタ配線、名前付きパイプ実 I/O（CreateNamedPipe を原子的ロック・敗者はクライアント転送）を
配線する。sprint1/2 の ViewModel/状態機械を GUI に結線する（depends_on: [2]）。
must verify は系統A `ctest --preset x64-core-test` ＋ 系統B `cmake --build --preset x64-release`（/W4・警告エラー）。

検証戦略の核（spec.md「検証戦略」）に従い、GUI 配線判断のうち **wx/FS 非依存に切り出せる部分を controller へ
厚く寄せて**決定論ゲート（系統A）に乗せ、wx/Win32/実 FS の実配線は系統B（コンパイル＋リンク成立）で検証した。

## 変更ファイル一覧

新規（controller・wx 非依存ロジック＝pika_core に含め gtest 対象。系統A）:
- `C:/dev/pika_editor/src/controller/editor_view_model.h` / `.cpp` — Settings→Scintilla 配線パラメータ写像
  （`make_editor_config`: tab_width/use_tabs/eol_mode・原文を変えない方針）と `tab_title_for_path`
- `C:/dev/pika_editor/src/controller/dir_lister.h` / `.cpp` — 実 FS 列挙フルパス→workspace::Entry 正規化
  （`to_workspace_rel_path`・`normalize_entries`: 除外適用・root 越境遮断・重複畳み込み。build_tree へ供給）

新規（テスト。系統A）:
- `C:/dev/pika_editor/tests/controller/editor_view_model_test.cpp` — 13 ケース（EditorConfig 写像・タブ見出し抽出）
- `C:/dev/pika_editor/tests/controller/dir_lister_test.cpp` — 9 ケース（相対化・大小無視・越境遮断・除外・重複・build_tree 接続）

新規（プラットフォーム層＝実 OS/FS I/O。系統B/C）:
- `C:/dev/pika_editor/src/app/pipe_server.h` / `.cpp` — CreateNamedPipe(FILE_FLAG_FIRST_PIPE_INSTANCE) の
  原子的ロック・owner-only DACL（SDDL）・PIPE_REJECT_REMOTE_CLIENTS・受信リスナースレッド・敗者クライアント転送・SID 取得
- `C:/dev/pika_editor/src/app/dir_enumerator.h` / `.cpp` — std::filesystem の逐次 1 階層列挙（UTF-8 パス・シンボリックリンク非追従）

新規（UI 層＝wx 依存。系統B/C）:
- `C:/dev/pika_editor/src/ui/ui_messages.h` / `.cpp` — UI 文言の単一メッセージ定義（ID→日本語。design 10章 K9）
- `C:/dev/pika_editor/src/ui/editor_panel.h` / `.cpp` — wxStyledTextCtrl ラッパ（EditorConfig 適用・UTF-8 表示・原文非変換）
- `C:/dev/pika_editor/src/ui/file_tree_panel.h` / `.cpp` — wxDataViewCtrl＋カスタム wxDataViewModel（TreeRowVm 描画）
- `C:/dev/pika_editor/src/ui/main_frame.h` / `.cpp` — MainFrame（メニュー/左ツリー/タブ/通知バー領域/メイン/右下ステータス・TabManager 結線）

変更:
- `C:/dev/pika_editor/src/app/main_gui.cpp` — スタブ→実体化（CLI 受領→単一インスタンス判定→サーバー公開→MainFrame 表示→列挙）
- `C:/dev/pika_editor/src/CMakeLists.txt` — pika_core に新 controller 2 モジュール（4 ソース）を追加（GUI ブロック外＝PIKA_BUILD_GUI 非依存）
- `C:/dev/pika_editor/src/app/CMakeLists.txt` — pika_ui 静的lib（src/ui＋dir_enumerator・wx::stc/aui 結線）新設＋pika exe に pipe_server を追加
- `C:/dev/pika_editor/tests/CMakeLists.txt` — pika_tests に新規 2 テストソースを追加

注: `dev/state.json` の差分は run-dev のループ状態であり本実装の書き込み対象ではない。`src/core/`・`docs/`・
spec.md・sprints.json・ref-dev・eval JSON は一切改変していない（`git diff --stat` で確認）。

## must criteria の実装状況

1. **main_gui 実体化＋MainFrame レイアウト骨格（メニュー/左ツリー/タブ/通知バー領域/メイン/右下固定ステータス。非オーバーレイ）** — 達成（系統B）。
   `MainFrame::build_menu`（ファイル/表示/ヘルプ・Ctrl+O/Ctrl+W）＋`build_layout`（通知バー領域→`wxSplitterWindow`〔左 FileTreePanel｜右 wxAuiNotebook〕→
   標準 `wxStatusBar` 2 ペイン）。ステータスは wxFrame 標準ステータスバー＝下端の専有領域でありビュー上にオーバーレイしない（airspace 問題を起こさない非オーバーレイ配置。design 10章）。
   `cmake --build --preset x64-release` で pika.exe 生成（後述）。

2. **ファイルツリー wxDataViewCtrl＋sprint1 TreeViewModel 入力＋逐次追加列挙** — 達成（系統A＋B）。
   `FileTreePanel`＝`wxDataViewCtrl`＋カスタム `wxDataViewModel`（`FileTreeModel`）。表示属性（状態マーク・アイコン分類）は
   controller `build_tree_view_model`（sprint1・gtest 済み）が決め、UI は TreeRowVm 木を描画するだけ。状態記号は単一定義
   `tree_view_messages::state_mark_symbol` から取得（色非依存）。逐次列挙の wx/FS 非依存部を `dir_lister::normalize_entries`
   へ切り出し gtest 検証（`NormalizeEntriesTest.FeedsBuildTree` で build_tree 接続・フォルダ先行/自然順を観測）。実 FS の 1 階層列挙は
   `dir_enumerator::list_directory`（表示後に呼ぶ。design 5.1 手順4）で UI をブロックしない構造。

3. **Scintilla 結線＋エンコーディング/改行反映＋SCI_SETTABWIDTH/SCI_SETUSETABS 明示で原文非変換** — 達成（系統A＋B）。
   配線判断（どの設定をどの SCI パラメータへ）を controller `make_editor_config` へ切り出し gtest 検証。`EditorPanel::apply_config` は
   `SetTabWidth`/`SetUseTabs`/`SetEOLMode`/`SetWrapMode`/`SetViewWhiteSpace` へ機械的に流すだけ。MainFrame は `util::read_all`→
   `util::decode_auto`（UTF-8 正規化・改行記録）の結果を `set_text_utf8` で表示し、改行・空白を変換しない（design 10章 G3）。
   テスト観測: `EditorConfigTest.ZeroTabWidthClampedToOne`（0 幅タブを安全側に 1 へ）・`InsertSpacesMeansUseTabsFalse`（論理反転）・
   `DefaultKeepsHardTabs`（既定はタブ文字を尊重＝原文の体裁を変えない）・`EolModeFollowsDetectedNewline`（Mixed は統一しない）。

4. **名前付きパイプ \\.\pipe\pika-<SID> 実 I/O（CreateNamedPipe 原子的ロック・敗者クライアント転送・サーバー公開は表示前）** — 達成（系統B/C）。
   `PipeServer::try_acquire`＝`CreateNamedPipeA(FILE_FLAG_FIRST_PIPE_INSTANCE)`（最初の 1 プロセスのみ作成成功＝OS 保証の原子的ロック・TOCTOU 回避）＋
   `make_owner_only_sddl` の SDDL から SECURITY_ATTRIBUTES＋`default_policy()` の PIPE_REJECT_REMOTE_CLIENTS/メッセージモード/8KB 上限。役割決定は
   `decide_instance`（sprint2・gtest 済み）。敗者は `send_to_server`（ERROR_PIPE_BUSY 短時間リトライ・受信は parse_request の信頼境界検証）で転送し
   OnInit=false＝終了コード0で終了（design 5.1 手順2「敗者は 0」）。main_gui は `start_listening` を **`frame_->Show(true)` の前に**呼ぶ
   （サーバー公開を表示前に完了）。受信はパイプスレッド→`CallAfter` で UI スレッドへマーシャリング（重い処理をスレッドでしない。design 4章）。

5. **cmake --build --preset x64-release コンパイル＋リンク成功（/W4・警告エラー扱いで exit 0）。controller を GUI から呼ぶ結線含む** — 達成。下記「自己実行した verify」参照。
   pika.exe／pika.com／pika_ui.lib／pika_tests.exe が /W4 /WX で警告エラーなしビルド成立。MainFrame は TabManager（sprint2）・
   make_editor_config/tab_title_for_path/normalize_entries/build_tree_view_model（sprint1/3）を呼ぶ。main_gui は plan_open/decide_instance（sprint2）を呼ぶ。

6. **ctest --preset x64-core-test 非回帰 PASS** — 達成。gtest 471 件 PASS / 0 失敗（sprint2 の 449 件は非回帰＋新規 22 件）。

## should criteria の実装状況

- **ステータス右下固定が WebView2/Scintilla 上で airspace を起こさない非オーバーレイ配置（ビュー高さを削る）** — 達成。
  wxFrame 標準 `wxStatusBar`（2 ペイン・右ペインに未読件数）を採用。標準ステータスバーは下端の専有領域でありビューにオーバーレイしないため、
  WebView2/Scintilla 上の airspace（HWND 重なり）問題を構造的に回避する（design 10章）。
- **起動→表示が最短経路（設定同期読み・状態読みのみ、ツリー列挙/監視は表示後に非同期）で 500ms を目標にできる構造** — 達成（構造のみ。実計測は系統C）。
  OnInit は CLI 解析→単一インスタンス判定→設定既定値→サーバー公開→`Show(true)` までを最短で行い、ワークスペース列挙
  （`open_workspace`→`enumerate_shallow_tree`）とタブオープンを **表示後**に呼ぶ。実 500ms 計測は GUI 実機が要るため docs/acceptance.md（sprint8）へ集約。
- **テーマ適用（ライト/ダーク/システム追従）の骨格＋ui-design 2章トークン配色** — 部分達成。`wxEVT_SYS_COLOUR_CHANGED` を束ね再描画する骨格を入れた
  （再起動なし追従。要件11.3）。配色トークンの実解決（テーマ ViewModel）は spec の系統A 対象だが本 sprint goal の主眼ではなく、spec/sprints が
  テーマ解決 ViewModel を sprint7 should（状態復元と同居）に置いているためそこで決定論化する（本 sprint は再適用フックの骨格に留める）。

## テスト化できなかった criteria とその理由

- **must 1（MainFrame レイアウト骨格）・must 4（パイプ実 I/O の実挙動）・must 2/3 の実描画/実編集** — wx ウィンドウ生成・実 Win32 パイプ I/O・
  Scintilla 実描画は GUI 実機/実 OS が要りユニットテスト不能（design 13章「UI 自動テストは初期版で持たない」）。spec の検証戦略どおり
  **系統B（コンパイル＋リンク成立＝`cmake --build --preset x64-release` exit 0）**で配線の型整合・シンボル解決を検証し、視覚・実挙動は
  系統C（docs/acceptance.md・sprint8 で整備）へ委ねた。判断ロジック（Scintilla 配線決定・列挙正規化・タブ状態機械・役割決定）は
  controller/core の wx/FS/Win32 非依存関数へ切り出し、`editor_view_model`・`dir_lister`（新規 22 ケース）＋既存 sprint1/2 ケースで決定論観測している。
- **単一インスタンスの実 2 プロセス競合（FILE_FLAG_FIRST_PIPE_INSTANCE の実獲得競合・実転送）** — 実 OS の同時起動が要るため系統C。決定論側は
  `decide_instance`（sprint2 gtest）が pipe_acquired→役割／転送 JSON 組み立てを検証済みで、本 sprint はその bool に実 CreateNamedPipe の獲得成否を注入する境界配線のみを足した。

## 自己実行した verify の結果

- `cmake --preset x64-core-test` / `cmake --preset x64-release` — いずれも構成成功（Configuring/Generating done）。
- **`ctest --preset x64-core-test` — exit 0 / 100% passed（2/2 ctest：pika_tests_build＋pika_tests）。gtest 471 件 PASS / 0 失敗（70 スイート）。**
  新規 4 スイート＝EditorConfigTest 8・TabTitleTest 5・ToRelPathTest 4・NormalizeEntriesTest 5＝計 22 件、差し引き 449 件が sprint2 到達点の非回帰 PASS。
- **`cmake --build --preset x64-release` — exit 0（/W4 /WX 警告エラーなし）。pika.exe・pika.com・pika_ui.lib・pika_tests.exe を生成。**
  controller（sprint1/2/3）を GUI（MainFrame）と main_gui から呼ぶ結線がコンパイル＋リンク成立。

合否判定の正本は run-dev が別途実行する結果。上記は自己確認。

DONE: C:/dev/pika_editor/dev/turns/turn-3-1-generator.md
