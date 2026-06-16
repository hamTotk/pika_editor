#include "controller/app_controller.h"

#include "core/ipc/path_normalizer.h"
#include "core/ipc/pipe_security.h"

namespace pika::controller
{

namespace
{

using core::ipc::ArgKind;
using core::ipc::ClassifiedArg;
using core::ipc::CliInvocation;
using core::ipc::CursorTarget;
using core::ipc::ExitCode;
using core::ipc::OpenTarget;
using core::ipc::ParseResult;
using core::ipc::ValidationResult;

// CursorTarget（has_position・1 始まり col 既定 1）を OpenTarget の
// line/column（0=指定なし）へ写す。 位置指定が無ければ line=0・column=0（IpcRequest
// スキーマの「指定なし」表現に合わせる）。
OpenTarget make_open_target(const std::string& abs_path, const CursorTarget& cur)
{
    OpenTarget t;
    t.path = abs_path;
    if (cur.has_position)
    {
        t.line = cur.line;
        t.column = cur.column;
    }
    return t;
}

} // namespace

OpenPlan plan_open(const CliContext& ctx, const core::ipc::PathProbe& probe)
{
    OpenPlan out;

    // 1) CLI 解析（design.md 5.1 手順1）。未知オプション等は GUI を起動せず不正引数で終了する。
    ParseResult parsed = core::ipc::parse_argv(ctx.args);
    if (parsed.errored)
    {
        out.accepted = false;
        out.exit_code = static_cast<int>(ExitCode::InvalidArgument);
        out.message = parsed.message;
        return out;
    }

    const CliInvocation& inv = parsed.invocation;
    out.goto_mode = inv.goto_mode;

    // help/version は GUI を起動しない受理（実際の出力はコンソールスタブが担う。GUI
    // は対象を持たない）。
    if (inv.show_help || inv.show_version)
    {
        out.accepted = true;
        out.exit_code = static_cast<int>(ExitCode::Accepted);
        return out;
    }

    // 2) 実在判定で検証・分類（存在しないフォルダはエラー・複数フォルダはエラー。要件3.1/3.2）。
    ValidationResult v = core::ipc::validate(inv, probe);
    if (!v.accepted)
    {
        out.accepted = false;
        out.exit_code = static_cast<int>(v.exit_code);
        out.message = v.message;
        return out;
    }

    // 3) 受理。各引数を cwd 基準で絶対パス化し（サーバー CWD 非依存。要件3.2）、
    //    フォルダはワークスペースへ、ファイル/新規ファイルはタブ対象へ振り分ける。
    out.accepted = true;
    out.exit_code = static_cast<int>(ExitCode::Accepted);

    for (const ClassifiedArg& arg : v.args)
    {
        const std::string abs_path = core::ipc::normalize_to_absolute(arg.path, ctx.cwd);
        if (arg.kind == ArgKind::ExistingFolder)
        {
            // 単一フォルダ方針（validate が複数を弾く）。ここでは 1 件だけが来る前提。
            out.folder = abs_path;
        }
        else
        {
            // ExistingFile / NewFile はタブで開く。-g 由来のカーソル位置を載せる。
            out.file_targets.push_back(make_open_target(abs_path, arg.target));
        }
    }

    // 引数なし（フォルダもファイルもなし）＝前回状態を復元する受理（要件3.1）。
    out.restore_previous = out.folder.empty() && out.file_targets.empty();
    return out;
}

InstanceDecision decide_instance(const InstanceContext& inst, const OpenPlan& plan)
{
    InstanceDecision out;
    out.pipe_name = core::ipc::make_pipe_name(inst.user_sid);

    if (inst.pipe_acquired)
    {
        // 原子的ロックを獲得＝サーバー兼ウィンドウ（design.md 5.1 手順2）。
        out.role = InstanceRole::Server;
        return out;
    }

    // 獲得失敗＝既存インスタンスあり。クライアントとして絶対パス化済み対象を転送する。
    out.role = InstanceRole::Client;
    out.client_exit_code = static_cast<int>(ExitCode::Accepted); // 敗者は受理＝0 で終了する。

    core::ipc::IpcRequest req;
    req.goto_mode = plan.goto_mode;
    req.targets = plan.file_targets;
    // フォルダ指定があればワークスペース切替の対象として転送する（行/桁は持たない）。
    if (!plan.folder.empty())
    {
        core::ipc::OpenTarget folder_target;
        folder_target.path = plan.folder;
        req.targets.push_back(folder_target);
    }
    out.transfer_json = core::ipc::serialize_request(req);
    return out;
}

std::vector<StartupStep> startup_sequence()
{
    // design.md 5.1 の順序を固定列で返す（順序の正典との一致を観測するため）。
    return {
        StartupStep::ResolveDataRoot, StartupStep::ParseCli,  StartupStep::DecideInstance,
        StartupStep::ShowWindow,      StartupStep::AsyncLoad,
    };
}

} // namespace pika::controller
