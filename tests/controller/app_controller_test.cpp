// controller/app_controller の検証（sprint2 must）。
// - CLI 受領: parse_argv/parse_goto 結果を OpenTarget 列（絶対パス・カーソル位置）へ正規化。
//   相対パスの絶対化を含めて観測する（要件3.2）。
// - 単一インスタンス判定: make_pipe_name/serialize_request を用いた役割決定と転送 JSON 組み立て。
// - 起動手順の順序が design.md 5.1 と一致（should）。
#include "controller/app_controller.h"

#include "core/ipc/ipc_message.h"
#include "core/ipc/pipe_security.h"

#include <gtest/gtest.h>

#include <set>

namespace
{

using pika::controller::CliContext;
using pika::controller::decide_instance;
using pika::controller::InstanceContext;
using pika::controller::InstanceRole;
using pika::controller::OpenPlan;
using pika::controller::plan_open;
using pika::controller::startup_sequence;
using pika::controller::StartupStep;
using pika::core::ipc::ExitCode;
using pika::core::ipc::IpcRequest;
using pika::core::ipc::parse_request;
using pika::core::ipc::PathProbe;

// テスト用 PathProbe: 与えた集合を実在ファイル/フォルダとみなす（FS 非依存で決定論）。
PathProbe make_probe(std::set<std::string> dirs, std::set<std::string> files)
{
    PathProbe p;
    p.is_dir = [dirs](const std::string& s) { return dirs.count(s) != 0; };
    p.is_file = [files](const std::string& s) { return files.count(s) != 0; };
    return p;
}

// ---- CLI 受領 → OpenTarget 正規化 ----

TEST(PlanOpenTest, RelativeFileIsAbsolutizedAgainstCwd)
{
    CliContext ctx;
    ctx.args = {"notes.md"};
    ctx.cwd = "C:\\work\\proj";
    // 実在ファイルとして cwd 連結後の絶対パスを実在判定に渡す。
    PathProbe probe = make_probe({}, {"notes.md"});

    OpenPlan plan = plan_open(ctx, probe);
    ASSERT_TRUE(plan.accepted);
    EXPECT_EQ(plan.exit_code, static_cast<int>(ExitCode::Accepted));
    EXPECT_TRUE(plan.folder.empty());
    ASSERT_EQ(plan.file_targets.size(), 1u);
    // 相対 "notes.md" が cwd 基準で絶対化されている。
    EXPECT_EQ(plan.file_targets[0].path, "C:\\work\\proj\\notes.md");
    EXPECT_EQ(plan.file_targets[0].line, 0);
    EXPECT_EQ(plan.file_targets[0].column, 0);
    EXPECT_FALSE(plan.restore_previous);
}

TEST(PlanOpenTest, ExistingFolderBecomesWorkspace)
{
    CliContext ctx;
    ctx.args = {"C:\\repo"};
    ctx.cwd = "C:\\work";
    PathProbe probe = make_probe({"C:\\repo"}, {});

    OpenPlan plan = plan_open(ctx, probe);
    ASSERT_TRUE(plan.accepted);
    EXPECT_EQ(plan.folder, "C:\\repo");
    EXPECT_TRUE(plan.file_targets.empty());
}

TEST(PlanOpenTest, GotoCarriesCursorPosition)
{
    CliContext ctx;
    ctx.args = {"-g", "C:\\src\\main.cpp:42:7"};
    ctx.cwd = "C:\\work";
    // -g の spec から位置を剥がした後のパスで実在判定する。
    PathProbe probe = make_probe({}, {"C:\\src\\main.cpp"});

    OpenPlan plan = plan_open(ctx, probe);
    ASSERT_TRUE(plan.accepted);
    EXPECT_TRUE(plan.goto_mode);
    ASSERT_EQ(plan.file_targets.size(), 1u);
    EXPECT_EQ(plan.file_targets[0].path, "C:\\src\\main.cpp");
    EXPECT_EQ(plan.file_targets[0].line, 42);
    EXPECT_EQ(plan.file_targets[0].column, 7);
}

TEST(PlanOpenTest, DriveLetterColonNotSplitAsPosition)
{
    // ドライブレター直後のコロンは位置指定として剥がさない（要件3.1）。
    CliContext ctx;
    ctx.args = {"-g", "C:\\a\\b.md"};
    ctx.cwd = "C:\\work";
    PathProbe probe = make_probe({}, {"C:\\a\\b.md"});

    OpenPlan plan = plan_open(ctx, probe);
    ASSERT_TRUE(plan.accepted);
    ASSERT_EQ(plan.file_targets.size(), 1u);
    EXPECT_EQ(plan.file_targets[0].path, "C:\\a\\b.md");
    EXPECT_EQ(plan.file_targets[0].line, 0); // 位置指定なし
}

TEST(PlanOpenTest, NonexistentFileIsAcceptedAsNewTab)
{
    CliContext ctx;
    ctx.args = {"C:\\new\\draft.md"};
    ctx.cwd = "C:\\work";
    PathProbe probe = make_probe({}, {}); // 実在しない

    OpenPlan plan = plan_open(ctx, probe);
    ASSERT_TRUE(plan.accepted); // 新規タブ扱いで受理（要件3.2）
    ASSERT_EQ(plan.file_targets.size(), 1u);
    EXPECT_EQ(plan.file_targets[0].path, "C:\\new\\draft.md");
}

TEST(PlanOpenTest, NoArgsRequestsRestorePrevious)
{
    CliContext ctx;
    ctx.cwd = "C:\\work";
    PathProbe probe = make_probe({}, {});

    OpenPlan plan = plan_open(ctx, probe);
    ASSERT_TRUE(plan.accepted);
    EXPECT_TRUE(plan.restore_previous); // 引数なし＝前回状態復元（要件3.1）
}

TEST(PlanOpenTest, MissingFolderIsRejected)
{
    CliContext ctx;
    ctx.args = {"C:\\nope\\"}; // 末尾区切り＝フォルダ意図かつ実在しない
    ctx.cwd = "C:\\work";
    PathProbe probe = make_probe({}, {});

    OpenPlan plan = plan_open(ctx, probe);
    EXPECT_FALSE(plan.accepted);
    EXPECT_EQ(plan.exit_code, static_cast<int>(ExitCode::InvalidArgument));
}

TEST(PlanOpenTest, UnknownOptionIsRejectedWithoutGui)
{
    CliContext ctx;
    ctx.args = {"--frobnicate"};
    ctx.cwd = "C:\\work";
    PathProbe probe = make_probe({}, {});

    OpenPlan plan = plan_open(ctx, probe);
    EXPECT_FALSE(plan.accepted);
    EXPECT_EQ(plan.exit_code, static_cast<int>(ExitCode::InvalidArgument));
}

TEST(PlanOpenTest, HelpIsAcceptedWithoutTargets)
{
    CliContext ctx;
    ctx.args = {"--help"};
    PathProbe probe = make_probe({}, {});

    OpenPlan plan = plan_open(ctx, probe);
    EXPECT_TRUE(plan.accepted);
    EXPECT_FALSE(plan.restore_previous);
    EXPECT_TRUE(plan.file_targets.empty());
}

// ---- 単一インスタンス判定 ----

TEST(DecideInstanceTest, AcquiredBecomesServerWithPipeName)
{
    InstanceContext inst;
    inst.pipe_acquired = true;
    inst.user_sid = "S-1-5-21-1-2-3-1001";

    OpenPlan plan;
    plan.accepted = true;

    auto d = decide_instance(inst, plan);
    EXPECT_EQ(d.role, InstanceRole::Server);
    EXPECT_EQ(d.pipe_name, pika::core::ipc::make_pipe_name(inst.user_sid));
    EXPECT_TRUE(d.transfer_json.empty()); // サーバーは転送 JSON を持たない
}

TEST(DecideInstanceTest, LoserBecomesClientAndBuildsTransferJson)
{
    InstanceContext inst;
    inst.pipe_acquired = false; // 既存インスタンスあり
    inst.user_sid = "S-1-5-21-9-9-9-1001";

    OpenPlan plan;
    plan.accepted = true;
    plan.goto_mode = true;
    pika::core::ipc::OpenTarget t;
    t.path = "C:\\src\\main.cpp";
    t.line = 42;
    t.column = 7;
    plan.file_targets.push_back(t);

    auto d = decide_instance(inst, plan);
    EXPECT_EQ(d.role, InstanceRole::Client);
    EXPECT_EQ(d.client_exit_code, static_cast<int>(ExitCode::Accepted)); // 敗者は 0 終了
    EXPECT_EQ(d.pipe_name, pika::core::ipc::make_pipe_name(inst.user_sid));
    ASSERT_FALSE(d.transfer_json.empty());
    EXPECT_EQ(d.transfer_json.find('\n'), std::string::npos); // 1 行 JSON

    // 転送 JSON が受信側スキーマで往復し、絶対パス・位置・goto が保たれる。
    IpcRequest back;
    ASSERT_TRUE(parse_request(d.transfer_json, back));
    EXPECT_TRUE(back.goto_mode);
    ASSERT_EQ(back.targets.size(), 1u);
    EXPECT_EQ(back.targets[0].path, "C:\\src\\main.cpp");
    EXPECT_EQ(back.targets[0].line, 42);
    EXPECT_EQ(back.targets[0].column, 7);
}

TEST(DecideInstanceTest, ClientTransfersFolderTarget)
{
    InstanceContext inst;
    inst.pipe_acquired = false;
    inst.user_sid = "S-1-5-21-1-1-1-500";

    OpenPlan plan;
    plan.accepted = true;
    plan.folder = "C:\\repo";

    auto d = decide_instance(inst, plan);
    ASSERT_EQ(d.role, InstanceRole::Client);

    IpcRequest back;
    ASSERT_TRUE(parse_request(d.transfer_json, back));
    ASSERT_EQ(back.targets.size(), 1u);
    EXPECT_EQ(back.targets[0].path, "C:\\repo"); // フォルダも絶対パスで転送される
}

// ---- fail-closed: セキュアな単一インスタンス保護を構築できない場合の縮退（要件3.2） ----

TEST(DecideInstanceTest, InsecureIsolationFallsBackToStandaloneServer)
{
    // SID 取得失敗・owner-only DACL 構築失敗等で per-user パイプ名/owner-only DACL を作れない場合、
    // owner-less パイプを公開する fail-open へ落とさず、IPC を一切張らないスタンドアロン起動
    // （主インスタンス＝Server・転送しない）へ縮退する。
    InstanceContext inst;
    inst.secure_isolation_available = false;
    inst.user_sid = ""; // SID 取得失敗を模す。

    OpenPlan plan;
    plan.accepted = true;
    pika::core::ipc::OpenTarget t;
    t.path = "C:\\src\\main.cpp";
    plan.file_targets.push_back(t);

    auto d = decide_instance(inst, plan);
    EXPECT_EQ(d.role, InstanceRole::Server); // 主インスタンスとして開く（クライアントにしない）
    EXPECT_TRUE(d.transfer_json.empty());    // 転送しない（存在しないサーバーへ送らない）
}

TEST(DecideInstanceTest, InsecureIsolationOverridesPipeAcquiredFalse)
{
    // pipe_acquired=false（通常ならクライアント）でも、セキュア化不能なら転送経路に落とさない。
    // この分岐がなければ敗者クライアントとして owner-less/不在サーバーへ転送しに行きハングしうる。
    InstanceContext inst;
    inst.secure_isolation_available = false;
    inst.pipe_acquired = false;
    inst.user_sid = "";

    OpenPlan plan;
    plan.accepted = true;
    plan.folder = "C:\\repo";

    auto d = decide_instance(inst, plan);
    EXPECT_EQ(d.role, InstanceRole::Server); // セキュア化不能は pipe_acquired より優先
    EXPECT_TRUE(d.transfer_json.empty());
}

TEST(DecideInstanceTest, SecureAvailableKeepsExistingClientBehavior)
{
    // 正常系（secure_isolation_available=true・既定値）は従来どおり pipe_acquired で分岐する
    // （セキュア化成功時の挙動は一切変えない）。
    InstanceContext inst;
    inst.pipe_acquired = false; // 既存インスタンスあり
    inst.user_sid = "S-1-5-21-7-7-7-1001";
    // secure_isolation_available は既定 true（明示せず正常系を確認）。

    OpenPlan plan;
    plan.accepted = true;
    pika::core::ipc::OpenTarget t;
    t.path = "C:\\a.md";
    plan.file_targets.push_back(t);

    auto d = decide_instance(inst, plan);
    EXPECT_EQ(d.role, InstanceRole::Client); // セキュア化成功時は従来どおりクライアント
    EXPECT_FALSE(d.transfer_json.empty());   // 転送 JSON を組み立てる
}

// ---- 起動手順の順序（should: design.md 5.1） ----

TEST(StartupSequenceTest, MatchesDesignOrder)
{
    auto seq = startup_sequence();
    ASSERT_EQ(seq.size(), 5u);
    EXPECT_EQ(seq[0], StartupStep::ResolveDataRoot);
    EXPECT_EQ(seq[1], StartupStep::ParseCli);
    EXPECT_EQ(seq[2], StartupStep::DecideInstance);
    EXPECT_EQ(seq[3], StartupStep::ShowWindow); // ウィンドウ表示は最短経路（手順3）
    EXPECT_EQ(seq[4], StartupStep::AsyncLoad);  // ツリー列挙・監視は表示後（手順4）
}

} // namespace
