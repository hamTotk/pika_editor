// ui/main_frame:
// メインウィンドウ（メニュー/左ツリー/タブバー/通知バー領域/メイン/右下ステータス）。
// design.md 2.1・10章（レイアウト・ステータス右下固定の非オーバーレイ配置）/ ui-design 7章 /
// 要件11章 / spec.md sprint3 must（レイアウト骨格・ツリー結線・Scintilla 結線）。
//
// MainFrame は controller（TabManager・TreeViewModel）と core（document の
// decode_auto・settings）を
// 結線し、プラットフォーム層（dir_enumerator・pipe_server）からのイベントを UI スレッドへ受ける。
// 判断ロジック自体は controller / core 側（gtest
// 済み）にあり、本クラスは配線とイベント中継に徹する。
#pragma once

#include "app/watch_thread.h"
#include "controller/diff_mode_model.h"
#include "controller/document_controller.h"
#include "controller/notification_model.h"
#include "controller/restore_plan.h"
#include "controller/shortcut_table.h"
#include "controller/tab_manager.h"
#include "controller/workspace_controller.h"
#include "core/document/review_flow.h"
#include "core/ipc/ipc_message.h"
#include "core/settings/settings.h"
#include "core/watcher/fs_event.h"
#include "core/watcher/watcher_core.h"
#include "util/encoding.h"
#include "util/task_runner.h"

#include <wx/aui/auibook.h>
#include <wx/event.h>
#include <wx/frame.h>
#include <wx/timer.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class wxMenuItem;

namespace pika::ui
{

class FileTreePanel;
class EditorPanel;
class PreviewView;

class MainFrame : public wxFrame
{
  public:
    // data_root はデータルートの絶対パス（snapshots\<wsKey> の親。退避・ベースラインの保存先）。
    // 空のときは確認済み/退避フローを非活性にする（退避先が無いと破壊的操作を始めない。設計原則1）。
    MainFrame(const core::settings::Settings& settings, const std::string& data_root);
    ~MainFrame() override;

    // ワークスペースフォルダを開く（ツリー列挙を表示後に開始する。design 5.1 手順4）。
    // 絶対パス。空なら「フォルダ未オープン」の空状態を表示する。
    void open_workspace(const std::string& folder_abs);

    // 1 ファイルをタブで開く（絶対パス）。既存タブがあればアクティブにする（TabManager に委譲）。
    void open_file(const std::string& file_abs);

    // 単一インスタンス転送/起動で受け取った開く対象を反映する（パイプ受信→UI スレッド・起動経路）。
    // 各 target の line>0 なら開いた直後のアクティブエディタへ行ジャンプする（-g。要件3.1/3.4）。
    // goto_source は -g 由来でソース表示固定の意図（行確認用途）。true なら表示モードを Source
    // にする。
    void apply_open_targets(const std::vector<core::ipc::OpenTarget>& targets,
                            bool goto_source = false);

    // 引数なし起動の前回セッション復元（要件10.1・F1）。RestorePlan（controller・wx 非依存）を
    // 機械的に消費し、ウィンドウ位置・ワークスペース・タブ・キャレット/スクロール・表示モードを当てる。
    // 存在しないファイルはスキップ（落ちない＝データを失わない・設計原則1）。
    void restore_session(const controller::RestorePlan& plan);

    // settings.toml の初回読込＋監視開始（F3/F4・F-016）。main_gui が表示直後・タブを開く前に 1 回
    // 呼ぶ（開くエディタへ正しい設定を乗せる。design 5.1）。pika は settings.toml
    // を読み取り専用で扱う。
    void load_and_watch_settings();

  private:
    void build_menu();
    void build_layout();
    void refresh_tree();
    void update_status();

    // settings.toml の読込・監視・反映（F3/F4・F-016。read-only）。
    void reload_settings_from_disk();         // 初回＋監視検知時に呼ぶ（読込→apply→反映/通知）
    void reapply_settings_to_ui();            // 新 settings を開いている全エディタへ再適用する
    void on_settings_poll(wxTimerEvent& evt); // poll タイマ: settings.toml の変化検知
    // 開いた時点のベースライン確立＋起動時未読判定（design 5.1 手順4・9章。F-013）。
    void establish_baseline();

    // 監視配線（sprint4）。監視スレッドの生イベント/再同期合図を UI スレッドで受けて反映する。
    void start_watching(); // ワークスペースの監視（or ポーリング）を開始する
    void stop_watching();  // 監視を停止する（フォルダ切替・終了）
    void on_raw_event(const core::watcher::RawEvent& ev); // 生イベント→WatcherCore（UI スレッド）
    void on_resync_needed(app::ResyncReason reason);      // 再同期合図→resync→反映（UI スレッド）
    void drain_watcher();                                 // WatcherCore::poll→WorkspaceController
    // デバウンス窓経過後の確定ドレイン（バースト最後の1件を拾う）。
    void on_debounce_timer(wxTimerEvent& evt);
    void apply_fs_events(const std::vector<core::watcher::FsEvent>& events); // 反映共通処理
    // クリーンな開タブを外部変更後の新内容へ再読込する（D3。dirty タブは守る）。
    void reload_open_tab_if_clean(const std::string& abs);

    void on_open_folder(wxCommandEvent& evt);
    void on_close_tab(wxCommandEvent& evt);
    void on_exit(wxCommandEvent& evt);
    void on_about(wxCommandEvent& evt);
    void on_refresh(wxCommandEvent& evt); // F5（要件11.2）= オンデマンド再同期

    // ウィンドウ終了（X/Alt+F4/終了メニュー）。未保存タブがあれば確認し、キャンセルなら Veto する
    // （データを失わない。要件5.7・設計原則1）。
    void on_close_window(wxCloseEvent& evt);

    // 終了時に現在のセッション（窓・タブ・キャレット/スクロール・モード・ツリー展開・テーマ）を
    // state.json へ書き出す（要件10.1・F1）。data_root_ が空なら何もしない。
    // 失敗は握り潰し（終了を妨げない＝状態保存はベストエフォート・設計原則1）。
    void save_session_state();

    // 未保存タブの有無（終了/フォルダ切替の確認要否）。
    bool has_unsaved_tabs() const;

    // 未保存タブの保存/破棄/キャンセル確認。戻り値 true=継続してよい（保存済み or 破棄）、
    // false=中断（キャンセル or 保存失敗）。閉じる/終了の経路はこの結果で進む/止める。
    bool confirm_discard_unsaved(int tab_index); // 1 タブ分（閉じる経路）
    bool confirm_discard_all_unsaved();          // 全タブ分（終了経路）

    // index のタブを保存する（既存 on_save 経路を流用）。保存できたら true。
    bool save_tab(int tab_index);

    // 差分（sprint5）。モード（ソース/分割/プレビュー）と差分トグルを直交させて WebView2 へ反映。
    void on_set_mode_source(wxCommandEvent& evt);
    void on_set_mode_split(wxCommandEvent& evt);
    void on_set_mode_preview(wxCommandEvent& evt);
    void on_toggle_diff(wxCommandEvent& evt);
    // 現モード×差分トグルから描画面を解決し、共有 WebView2 へ再ナビゲートする（占有世代照合）。
    void update_preview();
    // 差分トグルの可否を再評価し、メニュー項目の Enable/Check とステータス文言へ反映する。
    void update_diff_toggle_state();
    // 指定パスに差分可能なベースライン（内容 object を持つ確認済みエントリ）があるか
    // index.json で判定する（ハッシュのみ/未確認は false。design D2）。UI スレッド同期ロード。
    bool has_diffable_baseline(const std::string& abs) const;
    // プレビュー内リンクの振り分け（相対 .md/.html はタブ・他は既定ブラウザ。design 6章）。
    void on_preview_navigate(const std::string& url);

    // 中心体験④『確認済みにする』と保存・衝突退避（sprint6。判断は
    // controller::DocumentController）。
    void on_save(wxCommandEvent& evt); // Ctrl+S（design 5.3 保存・衝突退避）
    // 指定エンコーディング/BOM で 1 タブを保存する（prepare_save→encode→write_atomic→退避/通知/
    // マーク更新まで。design 5.3 の不変条件を 1 経路に集約）。BlockedEncoding を救済で UTF-8 へ切替
    // 保存する経路と通常保存が同じ手順を共有する。戻り値は
    // SaveDecision（呼び出し側が後続判断に使う）。
    controller::SaveDecision perform_save(EditorPanel* editor, const std::string& abs,
                                          const std::string& rel, util::Encoding encoding,
                                          bool with_bom);
    void on_confirm(wxCommandEvent& evt);     // 確認済みにする（design 5.4）
    void on_confirm_all(wxCommandEvent& evt); // すべて確認済みにする（design 5.4・J6）
    void on_rollback(wxCommandEvent& evt);    // 確認済み時点に戻す（巻き戻し。design 5.4）
    // フォーカス別ショートカット（Ctrl+Enter 等）を dispatch_shortcut で振り分ける（design 10章
    // J3）。
    void on_char_hook(wxKeyEvent& evt);
    // 現在の入力フォーカスの文脈を判定する（ショートカット・ディスパッチの入力。design 10章 J3）。
    controller::FocusContext current_focus() const;
    // アクティブタブのファイルの退避可否種別（10MB以上・画像・機密で退避不能。要件9.2）。
    core::document::FileContentClass active_content_class() const;
    // ワークスペース相対パス（workspace_ 配下のファイルのみ確認済み/退避の対象。空＝対象外）。
    std::string rel_path_for(const std::string& abs) const;
    // アクティブタブのエディタ（無ければ nullptr）。
    EditorPanel* active_editor() const;
    // アクティブタブのファイル絶対パス（無ければ空文字。プレビュー種別/差分可否の分類に使う）。
    std::string active_file_path() const;
    void on_tree_file_activated(const std::string& rel_path);
    void on_notebook_page_changed(wxAuiNotebookEvent& evt);
    void on_notebook_page_close(wxAuiNotebookEvent& evt);
    // 最後の実タブを閉じる遅延処理（PAGE_CLOSE イベント処理外＝CallAfter で呼ぶ）。
    // イベント中に AddPage/DeletePage すると wxAuiNotebook 内部が壊れて落ちるため、構造変更を
    // クリーンなスタックへ逃がす（1→2→1。空ページを経由しない）。
    void close_last_real_tab_deferred(int page);
    void on_sys_colour_changed(wxSysColourChangedEvent& evt);

    // 空状態プレースホルダページ（最後の実タブを閉じても notebook を 0 ページにしない安全網）。
    // wxAuiNotebook は最後のページ削除で 0 ページになる内部処理でクラッシュするため、実タブが 0
    // 件に なる直前に閉じられない簡易パネルを末尾へ差し込む。TabManager
    // には登録しない見せ方専用ページで、 実タブが 1 枚でも開けば除去する＝実タブの index と
    // TabManager の index の同順は崩れない。
    void ensure_placeholder_page(); // 空状態プレースホルダを末尾へ追加（既にあれば何もしない）
    void remove_placeholder_page(); // 空状態プレースホルダを除去（無ければ何もしない）
    bool active_page_is_placeholder() const; // 現在の選択ページがプレースホルダか

    // エディタの dirty 変化（Scintilla savepoint 通知）を TabManager の未保存フラグへ反映し、
    // タブ記号・ステータスを更新する（F-010。閉じ/終了確認の前提となる dirty 結線。設計原則1）。
    void on_editor_dirty_changed(const std::string& abs, bool dirty);

    // open_file の種別分岐（F-022。要件12.2・design 10章 B3）。種別判定は
    // controller::resolve_open_view （wx 非依存・gtest 済み）に委ね、ここは結果に応じて
    // EditorPanel／ImageViewPanel／ UnsupportedViewPanel のいずれかを notebook
    // の新規ページとして追加する結線だけを担う。 テキスト経路（EditorPanel への decode/dirty
    // 結線）を切り出した補助（従来の open_file 本体）。
    void open_text_editor_page(const std::string& file_abs, const std::string& title,
                               std::size_t idx);
    // 画像簡易ビュー（ImageViewPanel）を開く（要件12.2 I1。WebView2 を起動しない）。
    void open_image_page(const std::string& file_abs, const std::string& title, std::size_t idx);
    // 巨大画像/バイナリの非対応ビューを開く（種別ラベルを分けて UnsupportedViewPanel を追加する）。
    void open_unsupported_page(const std::string& file_abs, const std::string& title,
                               std::size_t idx, bool too_large_image);

    // タブ見出しの状態記号（削除済み ＞ 未保存 ＞ 差分あり）を display_mark から結線する（ui-design
    // 5章）。保存・確認・外部変更・ページ切替の各経路から呼んで SetPageText を更新する。
    void refresh_tab_title(std::size_t index); // 1 タブ分
    void refresh_all_tab_titles();             // 全タブ分（外部変更/一括確認の後）
    // TabState から「状態記号＋ファイル名」の表示文字列（UTF-8）を組み立てる。
    std::string tab_display_title(const controller::TabState& tab) const;

    // 失敗・スキップを通知バー集約 ViewModel（notification_model）へ載せて再描画する。
    // tab_path 空＝グローバル通知（タブ非依存）。detail 空なら種別の既定文言で表示する。
    void push_notification(controller::NotificationKind kind, const std::string& tab_path,
                           const std::string& detail);
    // HTML プレビューの JS 検知通知を更新する（検知時のみ JsDetected を 1 件・idempotent。B3）。
    void update_js_notification(const std::string& path, bool js_detected);
    // 同梱スクリプト（Mermaid/KaTeX/highlight.js）のブロック描画失敗件数を更新する（F-004・design
    // 6章 I1）。同一パスの古い RenderFailed を畳んでから count>0 のとき 1
    // 件積む（再描画で重複しない）。
    void update_render_failed_notification(const std::string& path, int count);
    // 通知集合をアクティブタブ文脈で集約し、通知バー領域へ反映する（最大3本＋他N件）。
    void refresh_notifications();

    core::settings::Settings settings_;
    std::string workspace_; // 現在開いているワークスペース（絶対パス・空＝未オープン）

    FileTreePanel* tree_ = nullptr;
    wxAuiNotebook* notebook_ = nullptr; // タブバー＋エディタ群
    // 空状態プレースホルダページ（実タブ 0 件のとき notebook を 0 ページにしない安全網。末尾固定・
    // TabManager 非登録）。実タブが 1 枚でも開けば除去するため、実タブ存在時は常に nullptr。
    wxWindow* placeholder_page_ = nullptr;
    wxWindow* notification_area_ = nullptr;
    wxStatusBar* status_ = nullptr;
    PreviewView* preview_ = nullptr; // 共有 1 枚 WebView2（遅延生成。sprint5）

    // 表示モード × 差分トグル（直交。ui-design 8章）。判断ロジックは controller::diff_mode_model。
    controller::ViewMode view_mode_ = controller::ViewMode::Source;
    bool diff_on_ = false;

    // update_preview の再入防止（安全網）。スプリッタの Split/Unsplit/Show が wxAuiNotebook の
    // PAGE_CHANGED を再発火し on_notebook_page_changed→update_preview を無限再帰させる経路を断つ
    // （最後のタブを閉じて空になる経路でスタックオーバーフロー）。RAII で確実に戻す。
    bool updating_preview_ = false;

    controller::TabManager tabs_; // タブ状態機械（wx 非依存・gtest 済み）
    // 外部変更の反映（sprint4。判断ロジックは wx 非依存・gtest 済み）。
    controller::WorkspaceController workspace_ctl_;
    std::unique_ptr<core::watcher::WatcherCore> watcher_; // 合成・自己保存抑制（非スレッドセーフ）
    std::unique_ptr<app::WatchThread> watch_thread_;      // ReadDirectoryChangesW 監視スレッド
    wxTimer debounce_timer_; // デバウンス窓経過後に poll を再実行する単発タイマー

    // A6（外部変更検知→反映）の計測（系統C A章・F-026）。最初の生イベント（on_raw_event）受領を
    // 起点としてスタンプし、変更が実際にエディタ/ツリーへ反映完了した時点（apply_fs_events で
    // 1 件以上の変更を反映しきった直後）を終点に区間 ms を出す。束で来る連続イベントの 2 件目
    // 以降は起点を上書きしない（最初の検知を基点にする）。観測専用＝既存挙動は変えない。
    bool perf_extchange_pending_ = false;

    // A2/A8 計測補助（系統C A章・F-026）。production では遊休サスペンド（DEC-02）を駆動する経路が
    // 無い（suspend_if_idle の呼び出し元ゼロ）ため、--perf-log 指定時に限り、プレビューが畳まれた
    // （Source モード＝WebView2 が非表示で TrySuspend が成功し得る）状態で一定アイドル後に 1 回だけ
    // suspend_if_idle を発火させる隠し計測手段。これで A8（サスペンド後アイドルメモリ）と、その後の
    // 再ナビゲートでの A2（Resume 再表示）が測れる。enabled() ガードで production 既定挙動は不変。
    wxTimer perf_idle_suspend_timer_;
    void on_perf_idle_suspend_timer(wxTimerEvent& evt);

    // settings.toml の poll 監視（F3/F4・F-016）。読み取り専用・低頻度変更のため軽量な poll で十分
    // （ReadDirectoryChangesW は使わない＝「軽い」原則）。前回観測した mtime/size と比較して変化時
    // のみ再読込する。settings_seen_ は初回 probe 済みフラグ（未配置→配置の検知に使う）。
    wxTimer settings_poll_timer_;
    bool settings_seen_ = false;
    std::uint64_t settings_mtime_ns_ = 0;
    std::uint64_t settings_size_ = 0;

    // 重い変換（render_markdown・差分計算）を UI スレッドから外すワーカー（design 4章）。
    util::TaskRunner tasks_;
    wxMenuItem* diff_item_ = nullptr; // 差分トグル項目（Enable/Check を更新するため保持）

    // 退避・ベースラインの保存先（データルート絶対パス。空＝退避先未確定で確認済み/退避フロー非活性）。
    std::string data_root_;
    // タブごとの保存衝突判定の素材（design 5.3）。読み込み時点の内容ハッシュ・エンコーディングを保持し、
    // 保存前に現ディスク内容ハッシュと突き合わせて衝突を検知する。キーはファイル絶対パス。
    struct DocMeta
    {
        std::string last_loaded_hash;                   // 読み込み時点の LF 正規化ハッシュ
        util::Encoding encoding = util::Encoding::Utf8; // 保存に使うエンコーディング
        bool has_bom = false;                           // BOM の有無（保存時に復元）
    };
    std::map<std::string, DocMeta> doc_meta_;

    // 通知バー集約（sprint7 の notification_model を GUI へ結線。design 10章 J1）。失敗・スキップは
    // ここへ積み、aggregate_notifications でアクティブタブ文脈に集約して通知バー領域へ描画する。
    std::vector<controller::Notification> notifications_;
    std::uint64_t notify_seq_ = 0; // 通知の投入順（同種・同一ファイルの最新判定。単調増加）
};

} // namespace pika::ui
