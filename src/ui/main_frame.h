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
#include "controller/shortcut_table.h"
#include "controller/tab_manager.h"
#include "controller/workspace_controller.h"
#include "core/document/review_flow.h"
#include "core/settings/settings.h"
#include "core/watcher/fs_event.h"
#include "core/watcher/watcher_core.h"
#include "util/encoding.h"
#include "util/task_runner.h"

#include <wx/aui/auibook.h>
#include <wx/event.h>
#include <wx/frame.h>
#include <wx/timer.h>

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

    // 単一インスタンス転送で受け取った開く対象を反映する（パイプ受信→UI スレッド）。
    // line/column は将来のカーソル移動用（本 sprint はファイルを開くまで）。
    void apply_open_targets(const std::vector<std::string>& file_abs_list);

  private:
    void build_menu();
    void build_layout();
    void refresh_tree();
    void update_status();

    // 監視配線（sprint4）。監視スレッドの生イベント/再同期合図を UI スレッドで受けて反映する。
    void start_watching(); // ワークスペースの監視（or ポーリング）を開始する
    void stop_watching();  // 監視を停止する（フォルダ切替・終了）
    void on_raw_event(const core::watcher::RawEvent& ev); // 生イベント→WatcherCore（UI スレッド）
    void on_resync_needed(app::ResyncReason reason);      // 再同期合図→resync→反映（UI スレッド）
    void drain_watcher();                                 // WatcherCore::poll→WorkspaceController
    // デバウンス窓経過後の確定ドレイン（バースト最後の1件を拾う）。
    void on_debounce_timer(wxTimerEvent& evt);
    void apply_fs_events(const std::vector<core::watcher::FsEvent>& events); // 反映共通処理

    void on_open_folder(wxCommandEvent& evt);
    void on_close_tab(wxCommandEvent& evt);
    void on_exit(wxCommandEvent& evt);
    void on_about(wxCommandEvent& evt);
    void on_refresh(wxCommandEvent& evt); // F5（要件11.2）= オンデマンド再同期

    // 差分（sprint5）。モード（ソース/分割/プレビュー）と差分トグルを直交させて WebView2 へ反映。
    void on_set_mode_source(wxCommandEvent& evt);
    void on_set_mode_split(wxCommandEvent& evt);
    void on_set_mode_preview(wxCommandEvent& evt);
    void on_toggle_diff(wxCommandEvent& evt);
    // 現モード×差分トグルから描画面を解決し、共有 WebView2 へ再ナビゲートする（占有世代照合）。
    void update_preview();
    // 差分トグルの可否を再評価し、メニュー項目の Enable/Check とステータス文言へ反映する。
    void update_diff_toggle_state();
    // プレビュー内リンクの振り分け（相対 .md/.html はタブ・他は既定ブラウザ。design 6章）。
    void on_preview_navigate(const std::string& url);

    // 中心体験④『確認済みにする』と保存・衝突退避（sprint6。判断は
    // controller::DocumentController）。
    void on_save(wxCommandEvent& evt);        // Ctrl+S（design 5.3 保存・衝突退避）
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
    void on_sys_colour_changed(wxSysColourChangedEvent& evt);

    core::settings::Settings settings_;
    std::string workspace_; // 現在開いているワークスペース（絶対パス・空＝未オープン）

    FileTreePanel* tree_ = nullptr;
    wxAuiNotebook* notebook_ = nullptr; // タブバー＋エディタ群
    wxWindow* notification_area_ = nullptr;
    wxStatusBar* status_ = nullptr;
    PreviewView* preview_ = nullptr; // 共有 1 枚 WebView2（遅延生成。sprint5）

    // 表示モード × 差分トグル（直交。ui-design 8章）。判断ロジックは controller::diff_mode_model。
    controller::ViewMode view_mode_ = controller::ViewMode::Source;
    bool diff_on_ = false;

    controller::TabManager tabs_; // タブ状態機械（wx 非依存・gtest 済み）
    // 外部変更の反映（sprint4。判断ロジックは wx 非依存・gtest 済み）。
    controller::WorkspaceController workspace_ctl_;
    std::unique_ptr<core::watcher::WatcherCore> watcher_; // 合成・自己保存抑制（非スレッドセーフ）
    std::unique_ptr<app::WatchThread> watch_thread_;      // ReadDirectoryChangesW 監視スレッド
    wxTimer debounce_timer_; // デバウンス窓経過後に poll を再実行する単発タイマー

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
};

} // namespace pika::ui
