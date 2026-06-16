// pika: GUI 本体（wxWidgets）。中心体験①『開く』のオーケストレーション（design.md 5.1）。
// sprint3: main_gui を実体化し、データルート解決 → CLI 受領 → 単一インスタンス判定（CreateNamedPipe
// の原子的ロック）→ サーバー公開（表示前）→ MainFrame 生成・表示 →
// 表示後にツリー列挙、の順序で起動する。
//
// 判断ロジック（データルート分岐・CLI 正規化・役割決定・転送 JSON 組み立て）は controller/core の
// wx/Win32 非依存関数（resolve_data_root・plan_open・decide_instance・serialize_request 他。gtest
// 済み） に置き、本ファイルは実 Win32（SID 取得・パイプ I/O・FS 実在判定）の注入と wx
// の起動制御に徹する。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <shlobj.h>

#include "app/pipe_server.h"
#include "controller/app_controller.h"
#include "controller/data_root.h"
#include "core/ipc/cli_parser.h"
#include "core/ipc/ipc_message.h"
#include "core/ipc/pipe_security.h"
#include "core/settings/settings.h"
#include "ui/main_frame.h"

#include <wx/app.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/string.h>

#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{

// FS 実在判定（プラットフォーム層の述語）をコアの PathProbe へ注入する（main_console と同方針）。
bool path_is_dir(const std::string& p)
{
    struct _stat st;
    return _stat(p.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
}
bool path_is_file(const std::string& p)
{
    struct _stat st;
    return _stat(p.c_str(), &st) == 0 && (st.st_mode & _S_IFREG) != 0;
}

// 自プロセスのカレントディレクトリ（UTF-8・絶対）。相対パスの絶対化基準（要件3.2）。
std::string current_working_dir()
{
    DWORD len = GetCurrentDirectoryW(0, nullptr);
    if (len == 0)
    {
        return std::string();
    }
    std::wstring w(len, L'\0');
    DWORD got = GetCurrentDirectoryW(len, w.data());
    w.resize(got);
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

// %LOCALAPPDATA%（通常版のデータルート親。UTF-8・末尾区切りなし）。取得不能なら空。
std::string local_app_data_dir()
{
    PWSTR path = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path) != S_OK || path == nullptr)
    {
        if (path != nullptr)
        {
            CoTaskMemFree(path);
        }
        return std::string();
    }
    std::wstring w(path);
    CoTaskMemFree(path);
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

// exe が置かれているディレクトリ（UTF-8・末尾区切りなし）。ポータブル判定・./pika-data の親。
std::string exe_dir()
{
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
    std::string out(exe.GetPath().ToUTF8().data());
    return out;
}

// データルートを解決する（design 5.1 手順1）。判断は controller::resolve_data_root（純ロジック・
// gtest 済み）に委ね、ここは実 FS（portable.txt の有無・既知フォルダ取得）を注入するだけにする。
std::string resolve_data_root_path()
{
    pika::controller::DataRootProbe probe;
    probe.exe_dir = exe_dir();
    probe.local_app_data = local_app_data_dir();
    probe.portable_marker_present = path_is_file(probe.exe_dir + "\\portable.txt");
    const pika::controller::DataRoot dr = pika::controller::resolve_data_root(probe);
    return dr.resolved ? dr.path : std::string();
}

// argv（wx が渡す引数）を UTF-8 std::string 列へ（プログラム名は除く）。
std::vector<std::string> collect_args(int argc, wxChar** argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        args.emplace_back(wxString(argv[i]).ToUTF8().data());
    }
    return args;
}

// 受理された OpenPlan からファイル対象の絶対パス列を取り出す（タブで開く対象）。
std::vector<std::string> file_paths(const pika::controller::OpenPlan& plan)
{
    std::vector<std::string> out;
    out.reserve(plan.file_targets.size());
    for (const auto& t : plan.file_targets)
    {
        out.push_back(t.path);
    }
    return out;
}

} // namespace

class PikaApp : public wxApp
{
  public:
    bool OnInit() override
    {
        namespace ctl = pika::controller;
        namespace ipc = pika::core::ipc;

        // 1. CLI 受領（design 5.1 手順1）。コアの純ロジックで正規化・検証する。
        ctl::CliContext cli;
        cli.args = collect_args(argc, argv);
        cli.cwd = current_working_dir();
        ipc::PathProbe probe;
        probe.is_dir = path_is_dir;
        probe.is_file = path_is_file;
        const ctl::OpenPlan plan = ctl::plan_open(cli, probe);
        if (!plan.accepted)
        {
            // 不正引数。GUI を起動せず終了する（検証は console スタブと同じコアロジック）。
            return false;
        }

        // 2. 単一インスタンス判定（design 5.1 手順2）。SID→パイプ名→CreateNamedPipe
        // の原子的ロック。
        const std::string sid = pika::app::current_user_sid();
        ctl::InstanceContext inst;
        inst.user_sid = sid;
        const std::string pipe_name = ipc::make_pipe_name(sid);
        const std::string owner_sddl = ipc::make_owner_only_sddl(sid);

        pipe_ = std::make_unique<pika::app::PipeServer>();
        inst.pipe_acquired = pipe_->try_acquire(pipe_name, owner_sddl);
        const ctl::InstanceDecision decision = ctl::decide_instance(inst, plan);

        if (decision.role == ctl::InstanceRole::Client)
        {
            // 敗者:
            // 引数を絶対パス化のうえ既存サーバーへ転送し、終了コード0で終了する（design 5.1）。
            pika::app::send_to_server(decision.pipe_name, decision.transfer_json);
            return false; // OnInit=false で wx を起動せず終了（exit code 0）。
        }

        // 3. 設定の同期読み（起動最序盤。design 5.1 手順1）。読み取り専用。
        pika::core::settings::Settings settings = pika::core::settings::default_settings();
        // settings.toml の実配置・監視配線は sprint7。本 sprint は既定値で起動する。

        // 4. サーバー公開（受信リスナー起動）を**ウィンドウ表示の前に**完了する（TOCTOU 回避。
        //    design 5.1 手順2「サーバー公開はウィンドウ表示前」）。受信は UI
        //    スレッドへマーシャリング。
        // データルート（退避・ベースラインの保存先）を解決して渡す（design 5.1 手順1。空なら退避
        // フローは非活性＝退避先未確定で破壊的操作を始めない。設計原則1）。
        const std::string data_root = resolve_data_root_path();
        frame_ = new pika::ui::MainFrame(settings, data_root);
        pipe_->start_listening([this](const std::string& line) {
            // パイプスレッドから来る。UI スレッドへ渡す（CallAfter）。
            pika::core::ipc::IpcRequest req;
            if (!pika::core::ipc::parse_request(line, req))
            {
                return; // 信頼境界: スキーマ不一致は破棄。
            }
            std::vector<std::string> files;
            for (const auto& t : req.targets)
            {
                files.push_back(t.path);
            }
            CallAfter([this, files]() { frame_->apply_open_targets(files); });
        });

        // 5. MainFrame 生成・表示（最短経路。design 5.1 手順3）。
        SetTopWindow(frame_);
        frame_->Show(true);

        // 6. 表示後にワークスペース列挙・タブを開く（design 5.1 手順4。表示をブロックしない）。
        if (!plan.folder.empty())
        {
            frame_->open_workspace(plan.folder);
        }
        const std::vector<std::string> files = file_paths(plan);
        if (!files.empty())
        {
            frame_->apply_open_targets(files);
        }
        return true;
    }

    int OnExit() override
    {
        if (pipe_)
        {
            pipe_->stop();
        }
        return wxApp::OnExit();
    }

  private:
    std::unique_ptr<pika::app::PipeServer> pipe_;
    pika::ui::MainFrame* frame_ = nullptr;
};

wxIMPLEMENT_APP(PikaApp);
