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

#include "controller/tab_manager.h"
#include "core/settings/settings.h"

#include <wx/aui/auibook.h>
#include <wx/frame.h>

#include <string>
#include <vector>

namespace pika::ui
{

class FileTreePanel;
class EditorPanel;

class MainFrame : public wxFrame
{
  public:
    explicit MainFrame(const core::settings::Settings& settings);

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

    void on_open_folder(wxCommandEvent& evt);
    void on_close_tab(wxCommandEvent& evt);
    void on_exit(wxCommandEvent& evt);
    void on_about(wxCommandEvent& evt);
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

    controller::TabManager tabs_; // タブ状態機械（wx 非依存・gtest 済み）
};

} // namespace pika::ui
