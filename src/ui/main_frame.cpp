#include "ui/main_frame.h"

#include "app/dir_enumerator.h"
#include "controller/dir_lister.h"
#include "controller/editor_view_model.h"
#include "controller/tree_view_model.h"
#include "core/watcher/fs_probe.h"
#include "core/watcher/resync.h"
#include "core/workspace/workspace_model.h"
#include "ui/editor_panel.h"
#include "ui/file_tree_panel.h"
#include "ui/ui_messages.h"
#include "util/atomic_file.h"
#include "util/encoding.h"

#include <wx/accel.h>
#include <wx/dirdlg.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/statusbr.h>

#include <optional>

namespace pika::ui
{

namespace
{

wxString u8(const std::string& s)
{
    return wxString::FromUTF8(s.c_str(), s.size());
}

wxString u8(MsgId id)
{
    return u8(message(id));
}

// ワークスペース直下の浅い木を列挙して TreeNode を組み立てる（逐次列挙の最初のバッチ。深い階層は
// フォルダ展開時に追加列挙＝本 sprint は 1 階層で骨格を満たす）。状態マークは WorkspaceController
// が保持する未読集合から付くため、TreeNode のみを返し ViewModel 化は呼び出し側で行う。
core::workspace::TreeNode enumerate_shallow_tree(const std::string& root_abs,
                                                 const core::settings::Settings& settings)
{
    std::vector<controller::RawListEntry> raw;
    app::list_directory(root_abs, "",
                        [&raw](const std::string&, std::vector<controller::RawListEntry> batch) {
                            raw = std::move(batch);
                        });
    const auto entries = controller::normalize_entries(root_abs, raw, settings.exclude);
    return core::workspace::build_tree(entries, settings.exclude);
}

} // namespace

MainFrame::MainFrame(const core::settings::Settings& settings)
    : wxFrame(nullptr, wxID_ANY, u8(MsgId::AppTitle), wxDefaultPosition, wxSize(1100, 720)),
      settings_(settings)
{
    build_menu();
    build_layout();
    update_status();
    // デバウンス窓経過後に poll を再実行し、バースト最後のイベントを確定させる（単発タイマー）。
    debounce_timer_.SetOwner(this);
    Bind(wxEVT_TIMER, &MainFrame::on_debounce_timer, this, debounce_timer_.GetId());
}

MainFrame::~MainFrame()
{
    stop_watching();
}

void MainFrame::build_menu()
{
    auto* menu_bar = new wxMenuBar();

    auto* file_menu = new wxMenu();
    file_menu->Append(wxID_OPEN, u8(MsgId::MenuOpenFolder));
    file_menu->Append(wxID_CLOSE, u8(MsgId::MenuClose));
    file_menu->AppendSeparator();
    file_menu->Append(wxID_EXIT, u8(MsgId::MenuExit));
    menu_bar->Append(file_menu, u8(MsgId::MenuFile));

    auto* view_menu = new wxMenu();
    // 再読み込み（F5）。監視不能環境/取りこぼし時のオンデマンド再同期（要件11.2・design 5.2）。
    view_menu->Append(wxID_REFRESH, u8(MsgId::MenuRefresh) + "\tF5");
    menu_bar->Append(view_menu, u8(MsgId::MenuView));

    auto* help_menu = new wxMenu();
    help_menu->Append(wxID_ABOUT, u8(MsgId::MenuAbout));
    menu_bar->Append(help_menu, u8(MsgId::MenuHelp));

    SetMenuBar(menu_bar);

    Bind(wxEVT_MENU, &MainFrame::on_open_folder, this, wxID_OPEN);
    Bind(wxEVT_MENU, &MainFrame::on_close_tab, this, wxID_CLOSE);
    Bind(wxEVT_MENU, &MainFrame::on_exit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::on_about, this, wxID_ABOUT);
    Bind(wxEVT_MENU, &MainFrame::on_refresh, this, wxID_REFRESH);
    Bind(wxEVT_SYS_COLOUR_CHANGED, &MainFrame::on_sys_colour_changed, this);
}

void MainFrame::build_layout()
{
    // ステータスバーは右下固定の非オーバーレイ配置（ビュー高さを削る。design 10章）。
    // wxFrame の標準ステータスバーは下端の専有領域でありオーバーレイしない＝airspace
    // 問題を起こさない。
    status_ = CreateStatusBar(2);
    int widths[2] = {-1, 240};
    status_->SetStatusWidths(2, widths);

    auto* root_sizer = new wxBoxSizer(wxVERTICAL);

    // 通知バー領域（最上段。最大3本＋他N件の集約は sprint7。本 sprint は領域だけ確保）。
    notification_area_ = new wxPanel(this, wxID_ANY);
    notification_area_->SetName(u8(MsgId::NotificationArea));
    notification_area_->SetMinSize(wxSize(-1, 0)); // 通知が無ければ高さ 0（コストゼロ）。
    root_sizer->Add(notification_area_, 0, wxEXPAND);

    // 左ツリー｜メイン（タブ）の水平分割。
    auto* splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                          wxSP_LIVE_UPDATE | wxSP_3DSASH);
    tree_ = new FileTreePanel(splitter);
    tree_->set_on_file_activated([this](const std::string& rel) { on_tree_file_activated(rel); });

    notebook_ = new wxAuiNotebook(splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_SCROLL_BUTTONS |
                                      wxAUI_NB_CLOSE_ON_ACTIVE_TAB);
    notebook_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &MainFrame::on_notebook_page_changed, this);
    notebook_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &MainFrame::on_notebook_page_close, this);

    splitter->SplitVertically(tree_, notebook_, 280);
    splitter->SetMinimumPaneSize(160);
    root_sizer->Add(splitter, 1, wxEXPAND);

    SetSizer(root_sizer);
}

void MainFrame::open_workspace(const std::string& folder_abs)
{
    // フォルダ切替: 既存の監視を畳んでから新ワークスペースを開く（design 5.6 の後始末に相当）。
    stop_watching();
    workspace_ = folder_abs;
    // 新ワークスペース用の WorkspaceController を作り直す（前フォルダの未読・引き継ぎ状態を破棄）。
    workspace_ctl_ = controller::WorkspaceController(workspace_);
    refresh_tree();
    update_status();
    // 表示後にツリー列挙・監視開始（design 5.1 手順4。表示をブロックしない）。
    start_watching();
}

void MainFrame::refresh_tree()
{
    if (workspace_.empty())
    {
        // フォルダ未オープンの空状態（中央文言は空状態 ViewModel〔sprint8〕で詳細化）。
        tree_->set_root(controller::TreeRowVm{});
        return;
    }
    const core::workspace::TreeNode root = enumerate_shallow_tree(workspace_, settings_);
    // 状態マーク（±/◆/取消線・伝播±淡）は WorkspaceController の未読集合から付与する（sprint4）。
    tree_->set_root(workspace_ctl_.build_view_model(root));
}

void MainFrame::start_watching()
{
    if (workspace_.empty())
    {
        return;
    }
    // WatcherCore（合成・自己保存抑制）。HashProbe は監視ルート相対パスの現ディスク内容ハッシュを
    // 返す（読めない/不在は nullopt）。自己保存判定の主条件＝内容ハッシュ一致（design 5.2）。
    const std::string root = workspace_;
    watcher_ = std::make_unique<core::watcher::WatcherCore>(
        [root](const std::string& rel) -> std::optional<std::uint64_t> {
            const auto h = core::watcher::content_hash_lf(root + "/" + rel);
            if (h.is_err())
            {
                return std::nullopt;
            }
            return h.value();
        });

    watch_thread_ = std::make_unique<app::WatchThread>();
    // 監視スレッドの生イベント/再同期合図は別スレッドから来る。CallAfter で UI スレッドへ渡す。
    watch_thread_->start(
        workspace_,
        [this](const core::watcher::RawEvent& ev) {
            CallAfter([this, ev]() { on_raw_event(ev); });
        },
        [this](app::ResyncReason reason) {
            CallAfter([this, reason]() { on_resync_needed(reason); });
        });
    // ポーリング間隔は WatchThread の既定（5秒。design 5.2）。設定化は settings 拡張時に行う。
    update_status();
}

void MainFrame::stop_watching()
{
    debounce_timer_.Stop();
    if (watch_thread_)
    {
        watch_thread_->stop();
        watch_thread_.reset();
    }
    watcher_.reset();
}

void MainFrame::on_raw_event(const core::watcher::RawEvent& ev)
{
    // UI スレッド。WatcherCore へ投入する。デバウンス窓内の連続書き込みは合成され窓経過後に確定する
    // ため、即時 poll で拾えない最後の 1 件を単発タイマーで拾う（design 5.2 のデバウンス整合）。
    if (!watcher_)
    {
        return;
    }
    watcher_->on_raw(ev);
    drain_watcher(); // 既に窓を越えた合成があれば即反映する。
    // 窓経過後にもう一度確定させる（デバウンス既定100ms＋余裕。多重 Start は前回をリセットする）。
    debounce_timer_.Start(150, wxTIMER_ONE_SHOT);
}

void MainFrame::on_debounce_timer(wxTimerEvent&)
{
    drain_watcher();
}

void MainFrame::drain_watcher()
{
    if (!watcher_)
    {
        return;
    }
    const auto now = static_cast<core::watcher::TimeMs>(::GetTickCount64());
    const std::vector<core::watcher::FsEvent> events = watcher_->poll(now);
    apply_fs_events(events);
}

void MainFrame::on_resync_needed(app::ResyncReason reason)
{
    // 再同期（オーバーフロー回復・ポーリング tick・F5）。全再列挙→突き合わせで取りこぼしを回復。
    (void)reason;
    if (workspace_.empty())
    {
        return;
    }
    // 進捗をステータスへ（design 10章 F3「F5/再同期中はその旨を表示」）。
    status_->SetStatusText(u8(MsgId::StatusSyncing), 0);

    // 保留中の合成/rename をドレインして二重計上を防いでから全再列挙する（design 5.2）。
    if (watcher_)
    {
        watcher_->drain_for_resync();
    }
    // WorkspaceController が保持するベースライン（mtime+size+ハッシュ）を突き合わせ基準に、
    // resync が全再列挙→差分を再構成する。無変化ファイルは未読化しない（取りこぼし回復。5.2）。
    // ベースライン本体の供給（起動時未読判定・確認済みでの更新）は core/snapshot 結線（sprint6）。
    const auto events = core::watcher::resync(workspace_, workspace_ctl_.baseline());
    apply_fs_events(events);
    update_status();
}

void MainFrame::apply_fs_events(const std::vector<core::watcher::FsEvent>& events)
{
    if (events.empty())
    {
        return;
    }
    const auto changes = workspace_ctl_.apply_events(events);
    for (const auto& c : changes)
    {
        // 削除は消失タブの安全遷移（バッファ保持・削除済み表示）。タブ側状態機械へ伝える。
        if (c.effect == controller::FsChangeEffect::PathRemoved && !workspace_.empty())
        {
            tabs_.mark_path_missing(workspace_ + "/" + c.path);
        }
    }
    // 未読集合が変わったのでツリーと未読件数を更新する。
    refresh_tree();
    update_status();
}

void MainFrame::open_file(const std::string& file_abs)
{
    const std::string title = controller::tab_title_for_path(file_abs);

    // 既存タブがあれば TabManager がアクティブ化して既存 index を返す（重複オープンを開かない）。
    const std::size_t before = tabs_.count();
    const std::size_t idx = tabs_.open(file_abs, title);
    if (idx < before)
    {
        // 既存タブ。ノートブックの該当ページをアクティブにする。
        if (idx < notebook_->GetPageCount())
        {
            notebook_->SetSelection(idx);
        }
        return;
    }

    // 新規タブ。ディスクから読み、core/util でエンコーディング/改行を判定して Scintilla
    // へ反映する。
    auto* editor = new EditorPanel(notebook_);
    const auto bytes = util::read_all(file_abs);
    if (bytes.is_ok())
    {
        // decode_auto は最後の砦として常に成功する（判定不能でも UTF-8 lossy で開く）。
        const auto decoded = util::decode_auto(bytes.value());
        util::DecodedText text;
        if (decoded.is_ok())
        {
            text = decoded.value();
        }
        else
        {
            text.content = bytes.value();
        }
        const auto cfg = controller::make_editor_config(settings_, text.newline);
        editor->apply_config(cfg);
        editor->set_text_utf8(text.content);
    }
    else
    {
        // 読み取り失敗は空エディタで開く（縮退表示の詳細は sprint8）。原文を勝手に作らない。
        editor->apply_config(controller::make_editor_config(settings_, util::Newline::None));
    }

    notebook_->AddPage(editor, u8(title), /*select*/ true);
    update_status();
}

void MainFrame::apply_open_targets(const std::vector<std::string>& file_abs_list)
{
    for (const auto& f : file_abs_list)
    {
        open_file(f);
    }
    // 転送を受けた既存ウィンドウは前面化する（単一インスタンス転送の UX。design 5.1）。
    Raise();
    RequestUserAttention();
}

void MainFrame::update_status()
{
    if (workspace_.empty())
    {
        status_->SetStatusText(u8(MsgId::StatusNoFolder), 0);
    }
    else if (watch_thread_ && watch_thread_->watching())
    {
        // 監視中（design 10章 F3「監視実行中はその旨を表示」）。
        status_->SetStatusText(u8(MsgId::StatusWatching), 0);
    }
    else if (watch_thread_)
    {
        // 監視不能環境でポーリングへフォールバック中。
        status_->SetStatusText(u8(MsgId::StatusPolling), 0);
    }
    else
    {
        status_->SetStatusText(u8(MsgId::StatusReady), 0);
    }
    // 右下: フォルダ内の未読ファイル数（WorkspaceController が保持・要件11章）。
    status_->SetStatusText(u8(status_unread(workspace_ctl_.unread().count())), 1);
}

void MainFrame::on_refresh(wxCommandEvent&)
{
    // F5（要件11.2）。監視スレッドへ手動再同期を要求する（同じ resync をオンデマンド実行）。
    if (watch_thread_)
    {
        watch_thread_->request_resync();
    }
}

void MainFrame::on_open_folder(wxCommandEvent&)
{
    wxDirDialog dlg(this, u8(MsgId::MenuOpenFolder), wxEmptyString,
                    wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
    {
        return;
    }
    open_workspace(std::string(dlg.GetPath().ToUTF8().data()));
}

void MainFrame::on_close_tab(wxCommandEvent&)
{
    const int sel = notebook_->GetSelection();
    if (sel != wxNOT_FOUND)
    {
        notebook_->DeletePage(static_cast<std::size_t>(sel));
        tabs_.close(static_cast<std::size_t>(sel));
        update_status();
    }
}

void MainFrame::on_exit(wxCommandEvent&)
{
    Close(true);
}

void MainFrame::on_about(wxCommandEvent&)
{
    wxMessageBox(u8(MsgId::AppTitle), u8(MsgId::MenuAbout), wxOK | wxICON_INFORMATION, this);
}

void MainFrame::on_tree_file_activated(const std::string& rel_path)
{
    if (workspace_.empty() || rel_path.empty())
    {
        return;
    }
    open_file(workspace_ + "/" + rel_path);
}

void MainFrame::on_notebook_page_changed(wxAuiNotebookEvent& evt)
{
    const int sel = notebook_->GetSelection();
    if (sel != wxNOT_FOUND)
    {
        tabs_.activate(static_cast<std::size_t>(sel));
    }
    evt.Skip();
}

void MainFrame::on_notebook_page_close(wxAuiNotebookEvent& evt)
{
    const int page = evt.GetSelection();
    if (page != wxNOT_FOUND)
    {
        tabs_.close(static_cast<std::size_t>(page));
    }
    evt.Skip();
}

void MainFrame::on_sys_colour_changed(wxSysColourChangedEvent& evt)
{
    // システムのライト/ダーク切替を再適用する骨格（テーマトークンの再解決は sprint7）。
    // 再起動なしで配色を追従させる（要件11.3）。
    Refresh();
    evt.Skip();
}

} // namespace pika::ui
