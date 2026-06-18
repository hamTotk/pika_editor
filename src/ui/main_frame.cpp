#include "ui/main_frame.h"

#include "app/dir_enumerator.h"
#include "controller/baseline_merge.h"
#include "controller/diff_mode_model.h"
#include "controller/dir_lister.h"
#include "controller/editor_view_model.h"
#include "controller/preview_builder.h"
#include "controller/tree_view_messages.h"
#include "controller/tree_view_model.h"
#include "core/diff/diff_engine.h"
#include "core/ipc/path_normalizer.h"
#include "core/render/html_inspector.h"
#include "core/render/html_sanitizer.h"
#include "core/render/markdown_renderer.h"
#include "core/snapshot/sensitive.h"
#include "core/snapshot/snapshot_store.h"
#include "core/snapshot/snapshot_types.h"
#include "core/watcher/fs_probe.h"
#include "core/watcher/resync.h"
#include "core/workspace/workspace_model.h"
#include "ui/editor_panel.h"
#include "ui/file_tree_panel.h"
#include "ui/preview_view.h"
#include "ui/ui_messages.h"
#include "util/atomic_file.h"
#include "util/encoding.h"
#include "util/hash.h"

#include <wx/accel.h>
#include <wx/button.h>
#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/menu.h>
#include <wx/menuitem.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/statusbr.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <optional>

namespace pika::ui
{

namespace
{

// 表示メニューの項目 ID（モード排他＝ラジオ、差分＝チェック。ui-design 8章）。
// 確認/保存/巻き戻し（design 5.3・5.4・10章 J3/J6）の ID もここに集約する。
enum
{
    ID_MODE_SOURCE = wxID_HIGHEST + 1,
    ID_MODE_SPLIT,
    ID_MODE_PREVIEW,
    ID_TOGGLE_DIFF,
    ID_CONFIRM,
    ID_CONFIRM_ALL,
    ID_ROLLBACK,
};

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

// 同梱アセット（preview.css 等）のディレクトリ＝exe 隣の assets/（UTF-8 絶対パス・'/' 区切り）。
// app.pika 仮想ホストの配信元になる（ユーザーのワークスペースではなく pika インストール領域）。
std::string asset_dir()
{
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
    wxFileName dir(exe.GetPath(), wxEmptyString);
    dir.AppendDir("assets");
    std::string p(dir.GetPath().ToUTF8().data());
    for (char& c : p)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }
    return p;
}

} // namespace

MainFrame::MainFrame(const core::settings::Settings& settings, const std::string& data_root)
    : wxFrame(nullptr, wxID_ANY, u8(MsgId::AppTitle), wxDefaultPosition, wxSize(1100, 720)),
      settings_(settings), data_root_(data_root)
{
    build_menu();
    build_layout();
    update_status();
    // デバウンス窓経過後に poll を再実行し、バースト最後のイベントを確定させる（単発タイマー）。
    debounce_timer_.SetOwner(this);
    Bind(wxEVT_TIMER, &MainFrame::on_debounce_timer, this, debounce_timer_.GetId());
    // フォーカス別ショートカット（Ctrl+Enter 等）を全体で 1 か所に集約して捌く（design 10章 J3）。
    Bind(wxEVT_CHAR_HOOK, &MainFrame::on_char_hook, this);
    // 終了（X/Alt+F4/終了メニュー）の未保存確認・Veto を 1 か所で受ける（要件5.7・設計原則1）。
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::on_close_window, this);
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
    file_menu->Append(wxID_SAVE, u8(MsgId::MenuSave));
    file_menu->Append(wxID_CLOSE, u8(MsgId::MenuClose));
    file_menu->AppendSeparator();
    file_menu->Append(wxID_EXIT, u8(MsgId::MenuExit));
    menu_bar->Append(file_menu, u8(MsgId::MenuFile));

    // レビュー（中心体験④）：確認済み/すべて確認済み/巻き戻し（design 5.4・10章 J6）。
    auto* review_menu = new wxMenu();
    review_menu->Append(ID_CONFIRM, u8(MsgId::MenuConfirm));
    review_menu->Append(ID_CONFIRM_ALL, u8(MsgId::MenuConfirmAll));
    review_menu->Append(ID_ROLLBACK, u8(MsgId::MenuRollback));
    menu_bar->Append(review_menu, u8(MsgId::MenuReview));

    auto* view_menu = new wxMenu();
    // 再読み込み（F5）。監視不能環境/取りこぼし時のオンデマンド再同期（要件11.2・design 5.2）。
    view_menu->Append(wxID_REFRESH, u8(MsgId::MenuRefresh) + "\tF5");
    view_menu->AppendSeparator();
    // モード（排他＝ラジオ）と差分トグル（独立＝チェック）を直交させる（ui-design 8章）。
    view_menu->AppendRadioItem(ID_MODE_SOURCE, u8(MsgId::MenuModeSource));
    view_menu->AppendRadioItem(ID_MODE_SPLIT, u8(MsgId::MenuModeSplit));
    view_menu->AppendRadioItem(ID_MODE_PREVIEW, u8(MsgId::MenuModePreview));
    view_menu->AppendSeparator();
    diff_item_ = view_menu->AppendCheckItem(ID_TOGGLE_DIFF, u8(MsgId::MenuToggleDiff));
    menu_bar->Append(view_menu, u8(MsgId::MenuView));

    auto* help_menu = new wxMenu();
    help_menu->Append(wxID_ABOUT, u8(MsgId::MenuAbout));
    menu_bar->Append(help_menu, u8(MsgId::MenuHelp));

    SetMenuBar(menu_bar);

    Bind(wxEVT_MENU, &MainFrame::on_open_folder, this, wxID_OPEN);
    Bind(wxEVT_MENU, &MainFrame::on_save, this, wxID_SAVE);
    Bind(wxEVT_MENU, &MainFrame::on_close_tab, this, wxID_CLOSE);
    Bind(wxEVT_MENU, &MainFrame::on_exit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::on_about, this, wxID_ABOUT);
    Bind(wxEVT_MENU, &MainFrame::on_refresh, this, wxID_REFRESH);
    Bind(wxEVT_MENU, &MainFrame::on_confirm, this, ID_CONFIRM);
    Bind(wxEVT_MENU, &MainFrame::on_confirm_all, this, ID_CONFIRM_ALL);
    Bind(wxEVT_MENU, &MainFrame::on_rollback, this, ID_ROLLBACK);
    Bind(wxEVT_MENU, &MainFrame::on_set_mode_source, this, ID_MODE_SOURCE);
    Bind(wxEVT_MENU, &MainFrame::on_set_mode_split, this, ID_MODE_SPLIT);
    Bind(wxEVT_MENU, &MainFrame::on_set_mode_preview, this, ID_MODE_PREVIEW);
    Bind(wxEVT_MENU, &MainFrame::on_toggle_diff, this, ID_TOGGLE_DIFF);
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

    // 通知バー領域（最上段。最大3本＋他N件の集約＝notification_model）。失敗・スキップの提示はここへ
    // 載せる（design 10章 J1・ui-design 10章）。通知が無ければ高さ 0（コストゼロ）で畳む。
    auto* area = new wxPanel(this, wxID_ANY);
    area->SetName(u8(MsgId::NotificationArea));
    area->SetSizer(new wxBoxSizer(wxVERTICAL));
    area->SetMinSize(wxSize(-1, 0));
    notification_area_ = area;
    root_sizer->Add(notification_area_, 0, wxEXPAND);

    // 左ツリー｜メイン（タブ）の水平分割。
    auto* splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                          wxSP_LIVE_UPDATE | wxSP_3DSASH);
    tree_ = new FileTreePanel(splitter);
    tree_->set_on_file_activated([this](const std::string& rel) { on_tree_file_activated(rel); });

    // メイン領域：エディタ（タブ）｜プレビュー（共有 1 枚 WebView2）の水平分割。分割モードで両方を
    // 出し、ソース/プレビューモードでは片側だけ（update_preview がサッシュを出し入れする）。
    auto* main_split = new wxSplitterWindow(splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                            wxSP_LIVE_UPDATE | wxSP_3DSASH);

    // C7: タブ溢れ時に全タブ一覧ドロップダウン（WINDOWLIST_BUTTON）を出す。
    // 隠れた未読タブのバッジ表示はカスタム wxAuiTabArt が必要なため今回はやらない（後回し）。
    notebook_ = new wxAuiNotebook(main_split, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_SCROLL_BUTTONS |
                                      wxAUI_NB_WINDOWLIST_BUTTON | wxAUI_NB_CLOSE_ON_ACTIVE_TAB);
    notebook_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &MainFrame::on_notebook_page_changed, this);
    notebook_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &MainFrame::on_notebook_page_close, this);

    preview_ = new PreviewView(main_split);
    preview_->set_on_navigate([this](const std::string& url) { on_preview_navigate(url); });
    // app.pika の同梱アセット（preview.css 等）の配信元＝exe 隣の assets/（design 6章 C6）。
    preview_->set_asset_dir(asset_dir());

    // 既定はソースモード（差分OFF）。プレビューは隠す（初回要求まで WebView2 を作らない）。
    main_split->SplitVertically(notebook_, preview_, -360);
    main_split->SetMinimumPaneSize(120);
    main_split->Unsplit(preview_); // ソースモードはエディタのみ

    splitter->SplitVertically(tree_, main_split, 280);
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
    // doc.pika の許可範囲＝ワークスペース配下に更新する（../ 抜けは遮断。design 6章 C7）。
    if (preview_)
    {
        preview_->set_document_root(workspace_);
    }
    // 中心体験の土台＝開いた時点のベースライン確立（design 5.1 手順4・9章・要件9.2。F-013）。
    establish_baseline();
    refresh_tree();
    update_status();
    // 表示後にツリー列挙・監視開始（design 5.1 手順4。表示をブロックしない）。
    start_watching();
}

void MainFrame::establish_baseline()
{
    if (workspace_.empty())
    {
        return;
    }
    // 確認済みの永続ベースライン（baselineHash 付き）を index.json からロードする
    // （on_confirm/perform_save と同じ要領）。data_root_ 未設定・load 失敗時は空 index で続行。
    core::snapshot::SnapshotIndex index;
    if (!data_root_.empty())
    {
        const std::string snapshots_root = data_root_ + "\\snapshots";
        core::snapshot::SnapshotStore store(snapshots_root,
                                            core::snapshot::workspace_key(workspace_));
        auto loaded = store.load();
        if (loaded.is_ok())
        {
            index = loaded.value();
        }
    }

    // 開いた時点の現 size+mtime をベースライン化（未確認はプレスクリーン任せ＝クリーン）。
    // 確認済みファイルは index の永続ベースライン（hash 付き）で上書きする。
    // 注: 起動時のハッシュ全計算はしない（未確認は hash_lf=0）。確認済みのみ後段の resync が
    // ハッシュ照合する。大規模フォルダ向けのバックグラウンド化は将来課題（現状は同期で十分軽い）。
    auto base = core::watcher::build_baseline_from_disk(workspace_);
    base = controller::merge_index_into_baseline(std::move(base), index);
    workspace_ctl_.set_baseline(base);

    // 起動時未読判定: 確認後に変わったファイルだけを未読化する（未確認はクリーン）。
    // F5（on_resync_needed）も同じ baseline を使うため、本結線だけで正常化する。
    const auto events = core::watcher::resync(workspace_, workspace_ctl_.baseline());
    apply_fs_events(events);
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
        if (workspace_.empty())
        {
            continue;
        }
        const std::string abs = workspace_ + "/" + c.path;
        switch (c.effect)
        {
        case controller::FsChangeEffect::PathRemoved:
            // 削除は消失タブの安全遷移（バッファ保持・削除済み表示）。タブ側状態機械へ伝える。
            tabs_.mark_path_missing(abs);
            break;
        case controller::FsChangeEffect::UnreadMarked:
            // 外部の作成/変更で未読化（タブは ± 差分あり／新規 ◆）。記号体系はツリーと一元化する。
            tabs_.set_unread(abs, true, /*has_baseline*/ !c.is_new);
            // クリーンな開タブは新内容へライブリロードする（D3 中心体験。未読バッジは維持）。
            reload_open_tab_if_clean(abs);
            break;
        case controller::FsChangeEffect::RenamedCarried:
            // rename 追従はパス同一性の付け替えであり、タブ側パスの更新は本タスクの範囲外（後続）。
            break;
        }
    }
    // 未読集合が変わったのでツリーと未読件数を更新する。タブの状態記号（削除済み/差分あり）も
    // 変わりうるので全タブ見出しを更新する（ui-design 5章）。
    refresh_tree();
    refresh_all_tab_titles();
    update_status();
}

void MainFrame::reload_open_tab_if_clean(const std::string& abs)
{
    // 開いているタブのうち「クリーン（ディスク一致＝未編集）」のものだけを再読込する。
    const std::size_t idx = tabs_.index_of(abs);
    if (idx == controller::TabManager::kNoActive || idx >= notebook_->GetPageCount())
    {
        return; // 開いていない（ツリー未読化のみ）。
    }
    auto* editor = dynamic_cast<EditorPanel*>(notebook_->GetPage(idx));
    if (!editor || editor->is_dirty())
    {
        // dirty タブは再読込しない（未保存編集を守る＝設計原則1）。
        // 衝突は保存時 prepare_save が退避（D4）。
        return;
    }

    // open_file の読込ロジックに倣う（read_all→decode_auto・失敗フォールバック）。
    const auto bytes = util::read_all(abs);
    if (!bytes.is_ok())
    {
        return; // 読めない（削除途中など）。古い内容を保持し、未読バッジだけ残す。
    }
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
    editor->reload_text_utf8(text.content);

    // 次回保存の衝突基準を新内容に合わせる（open_file の DocMeta 設定と同じ）。
    DocMeta meta;
    meta.last_loaded_hash = util::xxh3_64_lf_hex(text.content);
    meta.encoding = text.encoding;
    meta.has_bom = text.has_bom;
    doc_meta_[abs] = meta;

    // アクティブタブならプレビュー/差分面も新内容へ更新する。
    if (abs == active_file_path())
    {
        update_preview();
    }
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
        // 保存衝突判定の素材（読み込み時点のハッシュ・エンコーディング）を記録する（design 5.3）。
        DocMeta meta;
        meta.last_loaded_hash = util::xxh3_64_lf_hex(text.content);
        meta.encoding = text.encoding;
        meta.has_bom = text.has_bom;
        doc_meta_[file_abs] = meta;
    }
    else
    {
        // 読み取り失敗は空エディタで開く（縮退表示の詳細は sprint8）。原文を勝手に作らない。
        editor->apply_config(controller::make_editor_config(settings_, util::Newline::None));
    }

    notebook_->AddPage(editor, u8(title), /*select*/ true);
    // 追加直後に状態記号を反映する（新規タブは記号なしだが、経路を 1 本化する）。
    refresh_tab_title(idx);
    // dirty 結線（F-010）。set_text_utf8 が clean savepoint を張った後に束縛するので、初期ロード
    // で dirty=true は飛ばない（SAVEPOINTREACHED が来ても false で実害なし）。以後の編集で
    // savepoint を離れると未保存フラグ・タブ記号へ反映される（設計原則1の前提）。
    editor->set_on_dirty_changed(
        [this, file_abs](bool dirty) { on_editor_dirty_changed(file_abs, dirty); });
    update_status();
}

void MainFrame::on_editor_dirty_changed(const std::string& abs, bool dirty)
{
    // Scintilla の savepoint 通知 → TabManager の未保存フラグ → タブ記号（● 未保存）へ反映する。
    tabs_.set_unsaved(abs, dirty);
    refresh_tab_title(tabs_.index_of(abs));
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

EditorPanel* MainFrame::active_editor() const
{
    const int sel = notebook_->GetSelection();
    if (sel == wxNOT_FOUND)
    {
        return nullptr;
    }
    return dynamic_cast<EditorPanel*>(notebook_->GetPage(static_cast<std::size_t>(sel)));
}

void MainFrame::on_set_mode_source(wxCommandEvent&)
{
    view_mode_ = controller::ViewMode::Source;
    update_preview();
}

void MainFrame::on_set_mode_split(wxCommandEvent&)
{
    view_mode_ = controller::ViewMode::Split;
    update_preview();
}

void MainFrame::on_set_mode_preview(wxCommandEvent&)
{
    view_mode_ = controller::ViewMode::Preview;
    update_preview();
}

void MainFrame::on_toggle_diff(wxCommandEvent& evt)
{
    diff_on_ = evt.IsChecked();
    update_preview();
}

std::string MainFrame::active_file_path() const
{
    const std::size_t active = tabs_.active_index();
    if (active == controller::TabManager::kNoActive)
    {
        return {};
    }
    const controller::TabState* st = tabs_.at(active);
    return st ? st->path : std::string{};
}

std::string MainFrame::rel_path_for(const std::string& abs) const
{
    // workspace_ 配下のファイルのみ確認済み/退避の対象（'/' 区切りの相対パス）。範囲外は空を返す。
    if (workspace_.empty() || abs.size() <= workspace_.size())
    {
        return {};
    }
    if (abs.compare(0, workspace_.size(), workspace_) != 0 || abs[workspace_.size()] != '/')
    {
        return {};
    }
    return abs.substr(workspace_.size() + 1);
}

core::document::FileContentClass MainFrame::active_content_class() const
{
    // 退避可否の種別（要件9.2）。機密（.env 等）と 10MB 以上は内容 object を持てない＝退避不能。
    core::document::FileContentClass cls;
    const std::string abs = active_file_path();
    const std::string rel = rel_path_for(abs);
    cls.sensitive = !rel.empty() && core::snapshot::is_sensitive_default(rel);
    if (EditorPanel* editor = active_editor())
    {
        cls.content_object_allowed = editor->text_utf8().size() < core::snapshot::kContentSizeLimit;
    }
    return cls;
}

controller::FocusContext MainFrame::current_focus() const
{
    // 入力フォーカスを持つウィンドウからショートカット文脈を判定する（design 10章 J3・F6 循環）。
    // IsDescendant が非 const ポインタを要求するため focused も非 const で受ける（読み取りのみ）。
    wxWindow* focused = FindFocus();
    if (focused == nullptr)
    {
        return controller::FocusContext::Other;
    }
    if (dynamic_cast<const EditorPanel*>(focused) != nullptr)
    {
        return controller::FocusContext::Editor;
    }
    if (preview_ != nullptr && (focused == preview_ || preview_->IsDescendant(focused)))
    {
        // 共有 1 枚 WebView2 は差分とプレビューを兼ねる。現在の差分トグルで文脈を弁別する。
        return diff_on_ ? controller::FocusContext::DiffView : controller::FocusContext::Preview;
    }
    if (tree_ != nullptr && (focused == tree_ || tree_->IsDescendant(focused)))
    {
        return controller::FocusContext::Tree;
    }
    return controller::FocusContext::Other;
}

void MainFrame::on_char_hook(wxKeyEvent& evt)
{
    // フォーカス別ショートカットは controller::dispatch_shortcut が決める（wx 非依存・gtest
    // 済み）。 ここはキーイベントを KeyChord
    // へ写し、結果のアクションを実ハンドラへ振り分けるだけにする。
    controller::KeyChord chord;
    chord.ctrl = evt.ControlDown();
    chord.shift = evt.ShiftDown();
    chord.alt = evt.AltDown();
    chord.enter = (evt.GetKeyCode() == WXK_RETURN || evt.GetKeyCode() == WXK_NUMPAD_ENTER);

    switch (controller::dispatch_shortcut(current_focus(), chord))
    {
    case controller::ShortcutAction::Confirm: {
        wxCommandEvent e(wxEVT_MENU, ID_CONFIRM);
        on_confirm(e);
        return; // 既定動作（改行挿入等）を奪う＝確認に消費した。
    }
    case controller::ShortcutAction::ConfirmAll: {
        wxCommandEvent e(wxEVT_MENU, ID_CONFIRM_ALL);
        on_confirm_all(e);
        return;
    }
    case controller::ShortcutAction::None:
        break; // 割当なし＝既定動作へ委ねる（エディタの改行挿入等を奪わない）。
    }
    evt.Skip();
}

void MainFrame::on_save(wxCommandEvent&)
{
    // 保存・衝突退避（design 5.3）。実保存は perform_save に集約する（不変条件を 1 経路に閉じる）。
    EditorPanel* editor = active_editor();
    const std::string abs = active_file_path();
    const std::string rel = rel_path_for(abs);
    if (editor == nullptr || abs.empty() || data_root_.empty())
    {
        return;
    }

    // 読み込み時のエンコーディング/BOM で保存する（原文維持。design 5.3 手順5）。
    const auto mit = doc_meta_.find(abs);
    const util::Encoding enc = mit != doc_meta_.end() ? mit->second.encoding : util::Encoding::Utf8;
    const bool with_bom = mit != doc_meta_.end() && mit->second.has_bom;

    const controller::SaveDecision decision = perform_save(editor, abs, rel, enc, with_bom);
    if (decision != controller::SaveDecision::BlockedEncoding)
    {
        return; // Proceed=保存済み、その他 Blocked=perform_save 内で通知済み。
    }

    // 表現不能文字で中断した（C3・要件5.2）。無確認の文字欠落を起こさず、UTF-8 救済を選ばせる。
    // ファイルはまだ一切変更していない（perform_save は prepare_save 段で止まっている）。
    wxMessageDialog dlg(this, u8(MsgId::NotifyBlockedEncodingChoice), u8(MsgId::AppTitle),
                        wxYES_NO | wxICON_WARNING);
    dlg.SetYesNoLabels(u8(MsgId::SaveAsUtf8), u8(MsgId::ConfirmCancel));
    if (dlg.ShowModal() != wxID_YES)
    {
        return; // キャンセル＝保存中断（ファイルは無変更＝文字欠落なし）。
    }

    // UTF-8（BOM なし）へ切り替えて保存し直す。全 Unicode を表現でき BlockedEncoding は解消する。
    // 改行・退避・アトミック書き込みは perform_save の通常経路をそのまま通る（独自 write
    // は書かない）。
    perform_save(editor, abs, rel, util::Encoding::Utf8, false);
}

controller::SaveDecision MainFrame::perform_save(EditorPanel* editor, const std::string& abs,
                                                 const std::string& rel, util::Encoding encoding,
                                                 bool with_bom)
{
    // 判断は controller::DocumentController（wx 非依存・gtest 済み）に委ね、ここは index の
    // load→操作→save と実ディスク I/O・通知だけを担う。
    const std::string snapshots_root = data_root_ + "\\snapshots";
    // 退避結合の対象キー。ワークスペース配下なら workspace_key、単体ファイルは file_key
    // で同じ仕組みへ。
    const std::string ws_key =
        rel.empty() ? core::snapshot::file_key(abs) : core::snapshot::workspace_key(workspace_);
    core::snapshot::SnapshotStore store(snapshots_root, ws_key);
    controller::DocumentController doc(store);

    auto loaded = store.load();
    core::snapshot::SnapshotIndex index =
        loaded.is_ok() ? loaded.value() : core::snapshot::SnapshotIndex{};

    controller::SaveContext ctx;
    ctx.rel_path = rel.empty() ? abs : rel;
    ctx.buffer_content = editor->text_utf8();
    // 現ディスクの実内容を再読込してハッシュ再計算する（キャッシュ値を使わない。design 5.3
    // 手順1）。
    const auto disk = util::read_all(abs);
    ctx.disk_content = disk.is_ok() ? disk.value() : std::string{};
    const auto mit = doc_meta_.find(abs);
    ctx.last_loaded_hash = mit != doc_meta_.end() ? mit->second.last_loaded_hash : std::string{};
    ctx.encoding = encoding; // 保存に使うエンコーディング（救済時は UTF-8 を渡す）。
    ctx.cls = active_content_class();
    ctx.time = static_cast<std::int64_t>(::time(nullptr));

    const controller::SavePlan plan = doc.prepare_save(index, ctx);
    if (!plan.ok())
    {
        // 退避結合の Result を握り潰さず通知へ変換する（データを失わない。design 1章）。
        // BlockedEncoding は呼び出し側（on_save）が救済選択を出すため、ここでは通知せず decision
        // を返す。
        if (plan.decision == controller::SaveDecision::BlockedEncoding)
        {
            return plan.decision;
        }
        MsgId mid = plan.decision == controller::SaveDecision::BlockedUnstashable
                        ? MsgId::NotifyBlockedUnstashable
                        : MsgId::NotifyStashFailed;
        wxMessageBox(u8(mid), u8(MsgId::AppTitle), wxOK | wxICON_WARNING, this);
        return plan.decision;
    }

    // 退避が取れた（または衝突なし）＝アトミック置換に進む。改行は content の原文どおり維持される
    // （encode は改行を変換しない）。表現可能性は prepare_save で検査済み。
    const auto encoded = util::encode(ctx.buffer_content, encoding, with_bom);
    if (encoded.is_err())
    {
        wxMessageBox(u8(MsgId::NotifyBlockedEncoding), u8(MsgId::AppTitle), wxOK | wxICON_WARNING,
                     this);
        return controller::SaveDecision::BlockedEncoding;
    }
    const auto written = util::write_atomic(abs, encoded.value());
    if (written.is_err())
    {
        // I/O 失敗。旧内容は破壊しない（write_atomic の不変条件）。無言終了せず通知へ変換し、
        // ユーザーが『保存した』と誤認しないようにする（次の一手も併記。要件5.7）。
        wxMessageBox(u8(MsgId::NotifySaveIoFailed), u8(MsgId::AppTitle), wxOK | wxICON_WARNING,
                     this);
        push_notification(controller::NotificationKind::Conflict, abs,
                          message(MsgId::NotifySaveIoFailed));
        return controller::SaveDecision::BlockedStashFailed;
    }

    // incoming 退避が起きていれば index を保存しておく（退避 object のダングリングを防ぐ）。失敗は
    // 握り潰さず通知へ（退避 object 自体は保存済み＝データは失われていない。design 1章）。
    if (!plan.stash_hash.empty() && store.save(index).is_err())
    {
        push_notification(controller::NotificationKind::Conflict, abs,
                          message(MsgId::NotifyIndexSaveFailed));
    }

    // 保存後の状態を更新する（次回保存の衝突基準を保存内容へ。自己保存抑制は watcher 側で済む）。
    // 救済で UTF-8 へ切り替えたときは以降の保存も UTF-8 になるよう encoding/has_bom も書き戻す。
    if (mit != doc_meta_.end())
    {
        mit->second.last_loaded_hash = util::xxh3_64_lf_hex(ctx.buffer_content);
        mit->second.encoding = encoding;
        mit->second.has_bom = with_bom;
    }
    tabs_.set_unsaved(abs, false);
    // 自己保存トークンを登録する（書き込み直後）。watcher が自分の書き込みを外部変更として
    // 誤検知し未読化するのを防ぐ（design 5.2 自己保存抑制）。キーは監視ルート相対 rel・
    // ハッシュは書き込み済みディスク内容の LF 正規化値・時刻は poll と同じ単調クロックで揃える
    // （rel が空＝ワークスペース外単体は監視対象外なのでスキップ）。
    if (!rel.empty() && watcher_)
    {
        const auto self_hash = core::watcher::content_hash_lf(abs);
        if (self_hash.is_ok())
        {
            watcher_->register_self_save(rel, self_hash.value(),
                                         static_cast<core::watcher::TimeMs>(::GetTickCount64()));
        }
    }
    // Scintilla 内部の dirty もリセットして savepoint を張り直す（保存後の再編集で再び ● が付く）。
    editor->mark_clean();
    refresh_tab_title(tabs_.index_of(abs));
    if (plan.conflict)
    {
        // 衝突退避して上書きした旨は通知バーにも残す（ステータスは一過性。design 10章 J1）。
        push_notification(controller::NotificationKind::Conflict, abs,
                          message(MsgId::NotifyConflict));
    }
    status_->SetStatusText(plan.conflict ? u8(MsgId::NotifyConflict) : u8(MsgId::StatusSaved), 0);
    return controller::SaveDecision::Proceed;
}

void MainFrame::on_confirm(wxCommandEvent&)
{
    // 「確認済みにする」（design 5.4）。現ディスク内容でベースラインを更新し未読を解除する。退避結合の
    // Result を握り潰さず、失敗時は未読を維持して通知へ変換する（DocumentController が判断する）。
    const std::string abs = active_file_path();
    const std::string rel = rel_path_for(abs);
    if (abs.empty() || rel.empty() || data_root_.empty())
    {
        return;
    }

    const std::string snapshots_root = data_root_ + "\\snapshots";
    core::snapshot::SnapshotStore store(snapshots_root, core::snapshot::workspace_key(workspace_));
    controller::DocumentController doc(store);
    auto loaded = store.load();
    core::snapshot::SnapshotIndex index =
        loaded.is_ok() ? loaded.value() : core::snapshot::SnapshotIndex{};

    // 確認済みにする内容はディスクの実内容（確定直前に再読込。design 5.4 E2「見ていない内容を
    // ベースライン化しない」のための再照合の素材）。
    const auto disk = util::read_all(abs);
    if (disk.is_err())
    {
        return;
    }
    core::document::ReviewTarget target;
    target.rel_path = rel;
    target.content = disk.value();
    target.mtime = static_cast<std::int64_t>(::time(nullptr));
    target.cls = active_content_class();

    auto out = doc.confirm(index, workspace_ctl_.unread_mut(), target);
    if (out.is_err())
    {
        // 退避/更新失敗。未読は維持され、通知へ変換する（データを失わない）。
        wxMessageBox(u8(MsgId::NotifyStashFailed), u8(MsgId::AppTitle), wxOK | wxICON_WARNING,
                     this);
        return;
    }
    // index.json 保存（コミット相当）の失敗を握り潰さず通知へ変換する（design 1章・sprint6 趣旨）。
    if (store.save(index).is_err())
    {
        push_notification(controller::NotificationKind::Conflict, abs,
                          message(MsgId::NotifyIndexSaveFailed));
    }
    // ツリー/タブのマーク解除（未読集合から外れた）。
    tabs_.set_unread(abs, false, /*has_baseline*/ true);
    refresh_tab_title(tabs_.index_of(abs));
    refresh_tree();
    update_status();
    // 新ベースライン基準で差分面を即更新する（確認直後は差分なしになる。E3）。トグル有効状態も
    // 再評価される（diff_on_ は勝手に ON にしない＝ユーザー操作）。
    update_preview();
    status_->SetStatusText(u8(MsgId::StatusConfirmed), 0);
}

void MainFrame::on_confirm_all(wxCommandEvent&)
{
    // 「すべて確認済みにする」（design 5.4・J6）。開始時点の未読集合をフリーズして一括処理する。退避
    // 失敗/並行変化でスキップしたファイルは未読のまま残る（DocumentController
    // が握り潰さず分類する）。
    if (workspace_.empty() || data_root_.empty())
    {
        return;
    }
    const std::string snapshots_root = data_root_ + "\\snapshots";
    core::snapshot::SnapshotStore store(snapshots_root, core::snapshot::workspace_key(workspace_));
    controller::DocumentController doc(store);
    auto loaded = store.load();
    core::snapshot::SnapshotIndex index =
        loaded.is_ok() ? loaded.value() : core::snapshot::SnapshotIndex{};

    // 開始時点の未読集合をフリーズしてターゲット列を作る（freeze_hash＝現ディスク内容で並行変化検知）。
    std::vector<core::document::ReviewTarget> targets;
    for (const std::string& rel : workspace_ctl_.unread().items())
    {
        const auto disk = util::read_all(workspace_ + "/" + rel);
        if (disk.is_err())
        {
            continue; // 読めないファイルは確認しない（縮退）。
        }
        core::document::ReviewTarget t;
        t.rel_path = rel;
        t.content = disk.value();
        t.mtime = static_cast<std::int64_t>(::time(nullptr));
        t.cls.sensitive = core::snapshot::is_sensitive_default(rel);
        t.freeze_hash = util::xxh3_64_lf_hex(disk.value());
        targets.push_back(std::move(t));
    }

    const std::string batch_id = "batch-" + std::to_string(::time(nullptr));
    controller::ConfirmAllOutcome out =
        doc.confirm_all(index, workspace_ctl_.unread_mut(), targets, batch_id,
                        static_cast<std::int64_t>(::time(nullptr)));
    // index.json 保存（コミット相当）の失敗を握り潰さず通知へ変換する（design 1章・sprint6 趣旨）。
    if (store.save(index).is_err())
    {
        push_notification(controller::NotificationKind::Conflict, std::string{},
                          message(MsgId::NotifyIndexSaveFailed));
    }
    refresh_tree();
    refresh_all_tab_titles();
    update_status();
    // 全件完了の誤認防止: skipped>0 は即時提示する（並行変化/退避失敗で未確認が残る。要件8.3）。
    // 詳細（どのファイルが残ったか）はグローバル通知へ集約する（design 10章 J1）。
    if (out.skipped.empty())
    {
        status_->SetStatusText(u8(MsgId::StatusConfirmed), 0);
    }
    else
    {
        const std::string skipped_msg = notify_confirm_all_skipped(out.skipped.size());
        status_->SetStatusText(u8(skipped_msg), 0);
        push_notification(controller::NotificationKind::Conflict, std::string{},
                          message(MsgId::NotifyConfirmAllSkipped));
    }
}

void MainFrame::on_rollback(wxCommandEvent&)
{
    // 「確認済み時点に戻す」（design 5.4）。現ディスク内容を rollback
    // 退避し、ベースライン内容で上書き
    // する（退避が先＝失われる内容を必ず残す）。退避を取れない対象は Unsupported を通知へ変換する。
    const std::string abs = active_file_path();
    const std::string rel = rel_path_for(abs);
    if (abs.empty() || rel.empty() || data_root_.empty())
    {
        return;
    }
    const std::string snapshots_root = data_root_ + "\\snapshots";
    core::snapshot::SnapshotStore store(snapshots_root, core::snapshot::workspace_key(workspace_));
    controller::DocumentController doc(store);
    auto loaded = store.load();
    core::snapshot::SnapshotIndex index =
        loaded.is_ok() ? loaded.value() : core::snapshot::SnapshotIndex{};

    const auto disk = util::read_all(abs);
    if (disk.is_err())
    {
        return;
    }
    core::document::ReviewTarget target;
    target.rel_path = rel;
    target.content = disk.value();
    target.mtime = static_cast<std::int64_t>(::time(nullptr));
    target.cls = active_content_class();

    auto rolled = doc.rollback(index, target);
    if (rolled.is_err())
    {
        wxMessageBox(u8(MsgId::NotifyBlockedUnstashable), u8(MsgId::AppTitle),
                     wxOK | wxICON_WARNING, this);
        return;
    }
    // index.json 保存（コミット相当）の失敗を握り潰さず通知へ変換する（design 1章・sprint6 趣旨）。
    if (store.save(index).is_err())
    {
        push_notification(controller::NotificationKind::Conflict, abs,
                          message(MsgId::NotifyIndexSaveFailed));
    }
    // ベースライン内容でディスクを上書きしてバッファを再読込する（design 5.4）。
    const auto written = util::write_atomic(abs, rolled.value());
    if (written.is_err())
    {
        // 巻き戻しの書き戻し失敗を握り潰さず通知へ（見た目とディスクの不整合をユーザーに知らせる）。
        // 巻き戻し前の内容は rollback 退避に保存済み＝データ損失ではない旨も文言で示す（要件8.3）。
        wxMessageBox(u8(MsgId::NotifyRollbackWriteFailed), u8(MsgId::AppTitle),
                     wxOK | wxICON_WARNING, this);
        push_notification(controller::NotificationKind::Conflict, abs,
                          message(MsgId::NotifyRollbackWriteFailed));
        return;
    }
    if (EditorPanel* editor = active_editor())
    {
        editor->set_text_utf8(rolled.value());
    }
    auto mit = doc_meta_.find(abs);
    if (mit != doc_meta_.end())
    {
        mit->second.last_loaded_hash = util::xxh3_64_lf_hex(rolled.value());
    }
    // 巻き戻しで未保存編集が破棄され、ディスク＝ベースラインに揃った。タブの未保存記号を落とす。
    tabs_.set_unsaved(abs, false);
    refresh_tab_title(tabs_.index_of(abs));
    // 巻き戻し後の差分面を更新する（ディスク＝ベースラインなので差分なしになる）。
    update_preview();
}

bool MainFrame::has_diffable_baseline(const std::string& abs) const
{
    // 差分可能＝内容 object を持つ確認済みベースラインがあること（前回確認時点へ差分できる）。
    const std::string rel = rel_path_for(abs);
    if (data_root_.empty() || rel.empty())
    {
        return false;
    }
    const std::string snapshots_root = data_root_ + "\\snapshots";
    core::snapshot::SnapshotStore store(snapshots_root, core::snapshot::workspace_key(workspace_));
    auto loaded = store.load();
    if (loaded.is_err())
    {
        return false; // index 未作成/破損は安全側で非活性（差分しない）。
    }
    const core::snapshot::IndexEntry* e = loaded.value().find(rel);
    // baseline_object 空＝ハッシュのみ（10MB以上・画像・機密）。entry なし＝未確認。共に非活性。
    return e != nullptr && !e->baseline_object.empty();
}

void MainFrame::update_diff_toggle_state()
{
    if (!diff_item_)
    {
        return;
    }
    // 差分トグルの可否は controller::evaluate_diff_toggle が決める（wx 非依存・gtest 済み）。
    // ここは「現在状況を集めて渡し、結果を Enable/Check と理由文言へ反映する」だけにする。
    const std::string path = active_file_path();
    EditorPanel* editor = active_editor();
    controller::DiffToggleContext ctx;
    ctx.diffable_type = !path.empty() && controller::is_diffable_type(path);
    ctx.webview_available = PreviewView::webview_available();
    // ベースライン（前回確認時点）の有無を index.json から判定する。内容 object を持つ確認済み
    // エントリ（baseline_object 非空）のみ差分可。ハッシュのみ（10MB以上・画像・機密）や未確認は
    // false＝トグル無効のまま（design D2）。index.json は小さいので UI スレッド同期ロードで可
    // （重くなるなら将来キャッシュ化する）。
    ctx.has_baseline = has_diffable_baseline(path);
    ctx.content_bytes = editor ? editor->text_utf8().size() : 0;

    const controller::DiffDisableReason reason = controller::evaluate_diff_toggle(ctx);
    const bool enabled = (reason == controller::DiffDisableReason::None);
    diff_item_->Enable(enabled);
    if (!enabled)
    {
        // 無効化されたら強制オフにし、無言の空差分を出さない（ui-design 15章 Partial）。
        diff_on_ = false;
        diff_item_->Check(false);
        // 理由を左ステータスへ提示する（単一メッセージ定義経由。design 10章 K9・J1）。
        status_->SetStatusText(u8(std::string(controller::diff_disable_reason_label(reason))), 0);
    }
}

void MainFrame::update_preview()
{
    if (!preview_)
    {
        return;
    }
    // 差分トグルの可否を先に評価する（不可なら diff_on_ を落とす＝無言の空差分を防ぐ）。
    update_diff_toggle_state();

    // 描画面構成は controller::diff_mode_model が決める（wx 非依存・gtest 済み）。ここは結線のみ。
    const controller::PaneLayout layout = controller::resolve_pane_layout(view_mode_, diff_on_);
    auto* main_split = dynamic_cast<wxSplitterWindow*>(notebook_->GetParent());

    // サッシュの出し入れ：エディタとプレビューの両方が要るときだけ分割し、片方のみのときは畳む。
    if (main_split)
    {
        const bool need_both = layout.show_editor && layout.webview_active;
        // 単一ペイン間の遷移（プレビューのみ⇔ソースのみ）でも確実に出し分けるため、まず「分割」へ
        // 正規化してから不要側を Unsplit で畳む。Initialize による単一窓の差し替えは再描画/サイズ
        // 更新が安定せず、プレビュー単独で固まる（実機 F-003）。
        main_split->Freeze();
        if (!main_split->IsSplit())
        {
            main_split->SplitVertically(notebook_, preview_, -360);
        }
        notebook_->Show(true);
        preview_->Show(true);
        if (!need_both)
        {
            // プレビュー/差分のみならエディタを、ソースのみならプレビューを畳む。
            main_split->Unsplit(layout.webview_active ? static_cast<wxWindow*>(notebook_)
                                                      : static_cast<wxWindow*>(preview_));
        }
        main_split->Thaw();
    }
    if (!layout.webview_active)
    {
        return; // ソース（差分OFF）。WebView2 を作らない（遅延初期化を維持）。
    }

    EditorPanel* editor = active_editor();
    const std::string source = editor ? editor->text_utf8() : std::string{};
    // ファイル種別で JS 有効/無効を切替える（.html=JS無効・他=JS有効。design 6章 C5）。
    const std::string path = active_file_path();
    const controller::PreviewKind kind =
        path.empty() ? controller::PreviewKind::Markdown : controller::classify_preview_kind(path);

    // 占有鍵（タブ, モード, 差分ON）。タブが無ければ tab_id=0。design 4章の占有世代に対応。
    const auto tab_id = static_cast<std::uint64_t>(notebook_->GetSelection() + 1);
    const controller::OccupancyKey key{tab_id, view_mode_, diff_on_};
    // 要求時点で占有して stamp を確定する（ワーカー投入前。design 5.5 手順3）。
    const std::uint64_t stamp = preview_->request_occupy(key);

    const bool show_diff = layout.show_diff;
    const bool grid = layout.preview_diff_grid;

    // 差分旧側（前回確認時点）のベースライン読み出しに要る値をワーカー投入前にキャプチャする
    // （UI 状態をワーカーから触らない。実 I/O＝load/restore_baseline はワーカーで読み取りのみ）。
    const std::string rel = rel_path_for(path);
    const std::string snapshots_root =
        data_root_.empty() ? std::string{} : data_root_ + "\\snapshots";
    const std::string ws_key = core::snapshot::workspace_key(workspace_);

    // 重い変換（render_markdown・差分計算）をワーカーで実行し UI をブロックしない（design 4章）。
    // 完了後 UI スレッドへ戻し、stamp/key がまだ最新なら apply_document でナビゲートする。
    tasks_.submit([this, stamp, key, source, kind, show_diff, grid, path, rel, snapshots_root,
                   ws_key]() {
        // ワーカースレッド: コア（wx 非依存・スレッドセーフな純変換）だけを呼ぶ。
        controller::PreviewDoc doc;
        doc.kind = kind;
        if (kind == controller::PreviewKind::Html)
        {
            // HTML 文書はサニタイズ直送（Markdown 解釈しない）。インライン <style> は保持し、
            // <script>・危険 CSS のみ除去する（要件6.3/6.4・design 6章）。
            doc.body_html = core::render::sanitize_html(source);
        }
        else
        {
            const auto rendered = core::render::render_markdown(source);
            // 本文生成失敗時は白紙にせず最小フォールバック本文を出す（ui-design 15章 Error）。
            doc.body_html =
                rendered.is_ok()
                    ? rendered.value()
                    : "<p class=\"pika-placeholder\">プレビューを生成できませんでした</p>";
        }

        // HTML プレビューは JS 依存を検知して通知バー導線（既定ブラウザで開く）を出す。
        bool js_detected = false;
        if (kind == controller::PreviewKind::Html && !show_diff)
        {
            js_detected = core::render::inspect_html(source).depends_on_js();
        }

        std::string full_html;
        if (show_diff)
        {
            // 前回確認時点（ベースライン）の内容を旧側として読み出す（ワーカー内・読み取りのみ・
            // UI 非接触）。SnapshotStore はラムダ内でローカル生成し共有しない（並行ワーカー間で
            // 同一 store を触らない）。restore_baseline 失敗・内容なし（ハッシュのみ）は old 空＝
            // 全行追加表示にフォールバックする。
            std::string old_content;
            if (!snapshots_root.empty() && !rel.empty())
            {
                core::snapshot::SnapshotStore store(snapshots_root, ws_key);
                auto idx = store.load();
                if (idx.is_ok())
                {
                    auto b = store.restore_baseline(idx.value(), rel);
                    if (b.is_ok())
                    {
                        old_content = b.value();
                    }
                }
            }
            // 前回確認時点（ベースライン）→現内容の累積差分（コア DiffEngine・色非依存 +/-）。
            const core::diff::DiffEngine engine;
            const core::diff::DiffResult diff = engine.compute(/*old*/ old_content, /*new*/ source);
            full_html = grid ? controller::build_preview_diff_grid_document(doc, diff)
                             : controller::build_diff_document(
                                   diff, core::render::RemoteResourcePolicy::Blocked);
        }
        else
        {
            full_html = controller::build_preview_document(doc);
        }
        // 差分面は pika 生成の信頼済み HTML（JS 有効＝Markdown 相当）。プレビューは種別どおり。
        const controller::PreviewKind js_kind =
            show_diff && !grid ? controller::PreviewKind::Markdown : kind;
        CallAfter([this, stamp, key, full_html, js_kind, path, js_detected]() {
            preview_->apply_document(stamp, key, full_html, js_kind);
            update_js_notification(path, js_detected);
        });
    });
}

void MainFrame::on_preview_navigate(const std::string& url)
{
    // プレビュー内リンクの振り分け（design 6章・要件6.2/6.3）。相対 .md/.html はタブで開き、
    // 存在しなければ通知（新規空タブを作らない）。外部 http(s)/mailto は既定ブラウザへ委譲する。
    if (url.empty())
    {
        return;
    }
    // 委譲先スキームは許可リストで絞る（多層防御）。file:/UNC/カスタムプロトコルの自動起動を防ぐ。
    auto starts_with = [&url](const char* p) { return url.rfind(p, 0) == 0; };

    // 内部の同梱アセット・データ URI・about: は遷移対象でない（誤起動を防いで無視する）。
    if (starts_with("https://app.pika/") || starts_with("data:") || starts_with("about:"))
    {
        return;
    }

    // doc.pika 相対リンク：ワークスペース根（base href=doc.pika 根）基準でローカルパスへ解決し、
    // .md/.html はタブで開く。doc.pika の根は set_document_root(workspace_) と一致する（B7）。
    constexpr const char* kDocPrefix = "https://doc.pika/";
    if (starts_with(kDocPrefix))
    {
        std::string rel = url.substr(std::char_traits<char>::length(kDocPrefix));
        // クエリ（?g= 等）・フラグメント（#...）を落としてパス部分だけにする。
        const auto cut = rel.find_first_of("?#");
        if (cut != std::string::npos)
        {
            rel = rel.substr(0, cut);
        }
        if (rel.empty() || workspace_.empty())
        {
            return;
        }
        // 拡張子を小文字で取り出す（最後のセグメントのドット以降）。
        auto lower_ext = [](const std::string& s) {
            const auto dot = s.find_last_of('.');
            const auto sep = s.find_last_of("/\\");
            if (dot == std::string::npos || (sep != std::string::npos && dot < sep))
            {
                return std::string{};
            }
            std::string e = s.substr(dot + 1);
            for (char& c : e)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return e;
        };
        const std::string ext = lower_ext(rel);
        const bool is_doc =
            ext == "md" || ext == "markdown" || ext == "html" || ext == "htm" || ext == "txt";
        // ../ はワークスペース根でクランプされ範囲外へは出ない（path_normalizer・C7）。
        const std::string abs = core::ipc::normalize_to_absolute(rel, workspace_);
        const bool exists = wxFileName::FileExists(u8(abs));
        if (is_doc)
        {
            if (exists)
            {
                open_file(abs); // 既存タブならアクティブ化、無ければ新規タブ（重複は開かない）。
            }
            else
            {
                // 新規空タブを作らず、状態バーで知らせる（要件6.3 I5・データを汚さない）。
                status_->SetStatusText(u8(MsgId::StatusLinkNotFound), 0);
            }
            return;
        }
        // .md/.html 以外の相対リンクは既定ブラウザでローカルファイルを開く（design 6章）。
        if (exists)
        {
            wxString file_url = u8(abs);
            file_url.Replace("\\", "/");
            wxLaunchDefaultBrowser("file:///" + file_url);
        }
        else
        {
            status_->SetStatusText(u8(MsgId::StatusLinkNotFound), 0);
        }
        return;
    }

    if (starts_with("http://") || starts_with("https://") || starts_with("mailto:"))
    {
        wxLaunchDefaultBrowser(u8(url));
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
    if (sel == wxNOT_FOUND)
    {
        return;
    }
    // 未保存タブを無確認で閉じない（保存/破棄/キャンセル。要件5.7・設計原則1）。中断なら閉じない。
    if (!confirm_discard_unsaved(sel))
    {
        return;
    }
    notebook_->DeletePage(static_cast<std::size_t>(sel));
    tabs_.close(static_cast<std::size_t>(sel));
    update_status();
}

void MainFrame::on_exit(wxCommandEvent&)
{
    // 終了は wxEVT_CLOSE_WINDOW を発火させ、未保存確認・Veto を on_close_window に一本化する
    // （X/Alt+F4 と同じ経路。force=false で Veto を許す）。
    Close(false);
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
    // アクティブタブが変われば差分可否・プレビュー内容も変わる（種別/サイズ依存）ので再評価する。
    update_preview();
    // アクティブタブ固有の通知（衝突等）はタブ切替で表示対象が変わるので集約し直す。
    refresh_notifications();
    evt.Skip();
}

void MainFrame::on_notebook_page_close(wxAuiNotebookEvent& evt)
{
    const int page = evt.GetSelection();
    if (page == wxNOT_FOUND)
    {
        evt.Skip();
        return;
    }
    // ×/中クリック閉じも未保存なら確認する。キャンセル/保存失敗ならページ閉じイベントを Veto する
    // （wxAuiNotebook は Veto を尊重しページを残す。要件5.7・設計原則1）。
    if (!confirm_discard_unsaved(page))
    {
        evt.Veto();
        return;
    }
    // 実際の DeletePage は wxAuiNotebook 側が行う。TabManager をそれに同期させる。
    tabs_.close(static_cast<std::size_t>(page));
    evt.Skip();
}

void MainFrame::on_sys_colour_changed(wxSysColourChangedEvent& evt)
{
    // システムのライト/ダーク切替を再適用する骨格（テーマトークンの再解決は sprint7）。
    // 再起動なしで配色を追従させる（要件11.3）。
    Refresh();
    evt.Skip();
}

// ---- 終了/閉じるの未保存確認（データを失わない。要件5.7・設計原則1） ----

bool MainFrame::has_unsaved_tabs() const
{
    for (std::size_t i = 0; i < tabs_.count(); ++i)
    {
        const controller::TabState* st = tabs_.at(i);
        if (st != nullptr && st->unsaved)
        {
            return true;
        }
    }
    return false;
}

bool MainFrame::save_tab(int tab_index)
{
    if (tab_index < 0 || static_cast<std::size_t>(tab_index) >= tabs_.count())
    {
        return false;
    }
    const std::size_t idx = static_cast<std::size_t>(tab_index);
    // on_save
    // はアクティブタブを対象にするため、保存対象を一旦アクティブにしてから既存経路を流用する
    // （退避/アトミック置換/通知は on_save に一本化＝独自に書かない）。
    if (idx < notebook_->GetPageCount())
    {
        notebook_->SetSelection(idx);
        tabs_.activate(idx);
    }
    wxCommandEvent e(wxEVT_MENU, wxID_SAVE);
    on_save(e);
    // 保存成功は on_save が tabs_.set_unsaved(false) するため、保存後の未保存フラグで判定する。
    const controller::TabState* st = tabs_.at(idx);
    return st != nullptr && !st->unsaved;
}

bool MainFrame::confirm_discard_unsaved(int tab_index)
{
    if (tab_index < 0 || static_cast<std::size_t>(tab_index) >= tabs_.count())
    {
        return true; // 範囲外＝確認不要（そのまま進めてよい）。
    }
    const controller::TabState* st = tabs_.at(static_cast<std::size_t>(tab_index));
    if (st == nullptr || !st->unsaved)
    {
        return true; // 未保存でなければ確認不要。
    }
    // 保存/破棄/キャンセルの 3 択（モーダル）。既定は保存（誤操作でのデータ喪失を避ける）。
    wxMessageDialog dlg(this, u8(MsgId::ConfirmClosePrompt), u8(MsgId::AppTitle),
                        wxYES_NO | wxCANCEL | wxICON_WARNING);
    dlg.SetYesNoCancelLabels(u8(MsgId::ConfirmSave), u8(MsgId::ConfirmDiscard),
                             u8(MsgId::ConfirmCancel));
    switch (dlg.ShowModal())
    {
    case wxID_YES:
        // 保存できなければ閉じない（保存失敗で『保存した』誤認のまま破棄しない）。
        return save_tab(tab_index);
    case wxID_NO:
        return true; // 破棄して閉じる。
    default:
        return false; // キャンセル＝閉じない/終了しない。
    }
}

bool MainFrame::confirm_discard_all_unsaved()
{
    if (!has_unsaved_tabs())
    {
        return true;
    }
    wxMessageDialog dlg(this, u8(MsgId::ConfirmExitPrompt), u8(MsgId::AppTitle),
                        wxYES_NO | wxCANCEL | wxICON_WARNING);
    dlg.SetYesNoCancelLabels(u8(MsgId::ConfirmSaveAll), u8(MsgId::ConfirmDiscardExit),
                             u8(MsgId::ConfirmCancel));
    switch (dlg.ShowModal())
    {
    case wxID_YES:
        // 各未保存タブを既存 on_save 経路で保存する。1 つでも保存に失敗したら終了を中断する
        // （保存できなかった内容を黙って破棄しない。設計原則1）。
        for (std::size_t i = 0; i < tabs_.count(); ++i)
        {
            const controller::TabState* st = tabs_.at(i);
            if (st != nullptr && st->unsaved && !save_tab(static_cast<int>(i)))
            {
                return false;
            }
        }
        return true;
    case wxID_NO:
        return true; // 保存せず終了。
    default:
        return false; // キャンセル＝終了しない。
    }
}

void MainFrame::on_close_window(wxCloseEvent& evt)
{
    // X/Alt+F4/終了メニューの共通経路。未保存があれば確認し、キャンセル時は Veto して終了を止める。
    if (!confirm_discard_all_unsaved())
    {
        if (evt.CanVeto())
        {
            evt.Veto(); // 終了を中断（未保存内容を保持。要件5.7・設計原則1）。
            return;
        }
        // Veto できない強制終了（OS シャットダウン等）は止められない＝既存挙動で終了する。
    }
    stop_watching(); // 監視スレッドを畳んでから破棄する（デストラクタと二重でも安全）。
    Destroy();
}

// ---- タブ見出しの状態記号結線（display_mark。ui-design 5章） ----

std::string MainFrame::tab_display_title(const controller::TabState& tab) const
{
    const controller::StateMark mark = controller::display_mark(tab);
    // 削除済みは記号を持たない（ツリーは取り消し線）。タブは取り消し線を描けないため、色に依存せず
    // 記号で弁別できるよう接頭辞を付ける（ui-design 5章「記号・位置・取り消し線で全状態を弁別」）。
    if (mark == controller::StateMark::Deleted)
    {
        return "[削除] " + tab.title;
    }
    const std::string symbol(controller::state_mark_symbol(mark));
    if (symbol.empty())
    {
        return tab.title;
    }
    // 差分あり ±/新規 ◆ は名前の左（ui-design 5章の表「名前の左（タブ）」）。
    return symbol + " " + tab.title;
}

void MainFrame::refresh_tab_title(std::size_t index)
{
    const controller::TabState* st = tabs_.at(index);
    if (st == nullptr || index >= notebook_->GetPageCount())
    {
        return;
    }
    notebook_->SetPageText(index, u8(tab_display_title(*st)));
}

void MainFrame::refresh_all_tab_titles()
{
    const std::size_t n = std::min<std::size_t>(tabs_.count(), notebook_->GetPageCount());
    for (std::size_t i = 0; i < n; ++i)
    {
        refresh_tab_title(i);
    }
}

// ---- 通知バー集約の結線（notification_model。design 10章 J1） ----

void MainFrame::push_notification(controller::NotificationKind kind, const std::string& tab_path,
                                  const std::string& detail)
{
    controller::Notification n;
    n.kind = kind;
    n.tab_path = tab_path;
    n.seq = ++notify_seq_;
    n.detail = detail;
    notifications_.push_back(std::move(n));
    refresh_notifications();
}

void MainFrame::update_js_notification(const std::string& path, bool js_detected)
{
    // 同一ファイルの古い JS 検知通知を畳んでから検知時のみ 1 件積む（再プレビューでの重複防止）。
    notifications_.erase(std::remove_if(notifications_.begin(), notifications_.end(),
                                        [&](const controller::Notification& n) {
                                            return n.kind ==
                                                       controller::NotificationKind::JsDetected &&
                                                   n.tab_path == path;
                                        }),
                         notifications_.end());
    if (js_detected && !path.empty())
    {
        push_notification(controller::NotificationKind::JsDetected, path, std::string{});
    }
    else
    {
        refresh_notifications();
    }
}

void MainFrame::refresh_notifications()
{
    if (notification_area_ == nullptr)
    {
        return;
    }
    wxSizer* sizer = notification_area_->GetSizer();
    if (sizer == nullptr)
    {
        return;
    }
    // 既存の行を捨てて、現在のアクティブタブ文脈で集約し直す（純粋関数 aggregate_notifications）。
    notification_area_->DestroyChildren();
    sizer->Clear();

    const controller::NotificationView view =
        controller::aggregate_notifications(notifications_, active_file_path());

    for (const controller::NotificationRow& row : view.rows)
    {
        // detail があればそれを、無ければ種別の既定文言を出す（design 10章 K9）。
        const std::string text =
            row.detail.empty() ? notification_kind_label(row.kind) : row.detail;

        // JS 検知行は「既定のブラウザで開く」ボタンを併置する（B3。design 6章）。
        const bool js_row =
            row.kind == controller::NotificationKind::JsDetected && !row.tab_path.empty();
        if (js_row)
        {
            auto* line = new wxBoxSizer(wxHORIZONTAL);
            auto* label = new wxStaticText(notification_area_, wxID_ANY, u8(text));
            line->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxALL, 2);
            auto* btn = new wxButton(notification_area_, wxID_ANY, u8(MsgId::NotifyOpenInBrowser));
            const std::string target = row.tab_path;
            btn->Bind(wxEVT_BUTTON, [target](wxCommandEvent&) {
                // ローカル HTML を既定ブラウザで開く（パス区切りは file URL 用に正規化）。
                wxString u = wxString::FromUTF8(target.c_str(), target.size());
                u.Replace("\\", "/");
                wxLaunchDefaultBrowser("file:///" + u);
            });
            line->Add(btn, 0, wxALL, 2);
            sizer->Add(line, 0, wxEXPAND);
        }
        else
        {
            auto* label = new wxStaticText(notification_area_, wxID_ANY, u8(text));
            sizer->Add(label, 0, wxEXPAND | wxALL, 2);
        }
    }
    if (view.overflow > 0)
    {
        auto* more =
            new wxStaticText(notification_area_, wxID_ANY, u8(notify_overflow(view.overflow)));
        sizer->Add(more, 0, wxEXPAND | wxALL, 2);
    }
    // 通知が無ければ高さ 0 で畳む（コストゼロ）。あれば内容に合わせて再レイアウトする。
    notification_area_->SetMinSize(wxSize(-1, view.rows.empty() ? 0 : -1));
    notification_area_->Layout();
    Layout();
}

} // namespace pika::ui
