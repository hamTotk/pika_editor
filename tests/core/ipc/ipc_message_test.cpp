// core/ipc IPC スキーマ検証の検証（sprint7 must「IPC スキーマ検証」）。
// 受信 JSON が「1行・最大数KB・op=open のみ・絶対パスのみ・整数のみ」のスキーマに合わないデータを
// 破棄すること、受理操作がパスのオープンに限定されることを観測する（要件3.2 の信頼境界）。
#include "core/ipc/ipc_message.h"

#include <string>

#include <gtest/gtest.h>

namespace
{

using pika::core::ipc::IpcRequest;
using pika::core::ipc::is_absolute_windows_path;
using pika::core::ipc::kMaxMessageBytes;
using pika::core::ipc::OpenTarget;
using pika::core::ipc::parse_request;
using pika::core::ipc::serialize_request;

// ---- 絶対パス判定 ----

TEST(IpcAbsPath, DriveAbsolute)
{
    EXPECT_TRUE(is_absolute_windows_path("C:\\dir\\a.md"));
    EXPECT_TRUE(is_absolute_windows_path("c:/dir/a.md"));
}

TEST(IpcAbsPath, Unc)
{
    EXPECT_TRUE(is_absolute_windows_path("\\\\server\\share\\a.md"));
}

TEST(IpcAbsPath, RejectsRelativeAndDriveRelative)
{
    EXPECT_FALSE(is_absolute_windows_path("a.md"));
    EXPECT_FALSE(is_absolute_windows_path("dir\\a.md"));
    EXPECT_FALSE(is_absolute_windows_path("C:a.md")); // ドライブ相対は CWD 依存のため不可
    EXPECT_FALSE(is_absolute_windows_path(""));
}

// ---- 往復 ----

TEST(IpcMessage, RoundTrip)
{
    IpcRequest req;
    req.goto_mode = true;
    req.targets.push_back(OpenTarget{"C:\\proj\\a.md", 120, 5});
    req.targets.push_back(OpenTarget{"C:\\proj\\b.md", 0, 0});

    const std::string line = serialize_request(req);
    // 1 行であること（改行を含まない）。
    EXPECT_EQ(line.find('\n'), std::string::npos);
    EXPECT_EQ(line.find('\r'), std::string::npos);

    IpcRequest out;
    ASSERT_TRUE(parse_request(line, out));
    EXPECT_TRUE(out.goto_mode);
    ASSERT_EQ(out.targets.size(), 2u);
    EXPECT_EQ(out.targets[0].path, "C:\\proj\\a.md");
    EXPECT_EQ(out.targets[0].line, 120);
    EXPECT_EQ(out.targets[0].column, 5);
    EXPECT_EQ(out.targets[1].path, "C:\\proj\\b.md");
    EXPECT_EQ(out.targets[1].line, 0);
}

// ---- スキーマ違反の破棄 ----

TEST(IpcMessage, RejectsMalformedJson)
{
    IpcRequest out;
    EXPECT_FALSE(parse_request("{not json", out));
    EXPECT_FALSE(parse_request("", out));
    EXPECT_FALSE(parse_request("[]", out)); // ルートが配列
}

TEST(IpcMessage, RejectsWrongOp)
{
    // op が open でないものは破棄（受理操作をパスのオープンに限定。要件3.2）。
    IpcRequest out;
    EXPECT_FALSE(parse_request(R"({"op":"exec","targets":[]})", out));
    EXPECT_FALSE(parse_request(R"({"targets":[]})", out)); // op 欠落
}

TEST(IpcMessage, RejectsNonAbsolutePath)
{
    IpcRequest out;
    // 相対パスは破棄（クライアントが絶対パス化して転送する契約）。
    EXPECT_FALSE(parse_request(R"({"op":"open","targets":[{"path":"a.md"}]})", out));
}

TEST(IpcMessage, RejectsMissingPathField)
{
    IpcRequest out;
    EXPECT_FALSE(parse_request(R"({"op":"open","targets":[{"line":5}]})", out));
}

TEST(IpcMessage, RejectsNonIntegerLine)
{
    IpcRequest out;
    // line が文字列（非整数）はスキーマ違反として破棄。
    const std::string msg = R"({"op":"open","targets":[{"path":"C:\\a.md","line":"x"}]})";
    EXPECT_FALSE(parse_request(msg, out));
}

TEST(IpcMessage, RejectsTargetsNotArray)
{
    IpcRequest out;
    EXPECT_FALSE(parse_request(R"({"op":"open","targets":42})", out));
}

TEST(IpcMessage, RejectsOversizeMessage)
{
    // 最大数KB 超過は読まずに破棄（要件3.2）。
    std::string big = R"({"op":"open","targets":[{"path":"C:\\a.md"}]})";
    big.append(kMaxMessageBytes + 1, ' ');
    IpcRequest out;
    EXPECT_FALSE(parse_request(big, out));
}

TEST(IpcMessage, AcceptsEmptyTargets)
{
    // targets 空（パス無し転送＝前面化）は受理する。
    IpcRequest out;
    ASSERT_TRUE(parse_request(R"({"op":"open","targets":[]})", out));
    EXPECT_TRUE(out.targets.empty());
}

TEST(IpcMessage, RejectsNonBoolGoto)
{
    IpcRequest out;
    EXPECT_FALSE(parse_request(R"({"op":"open","goto":1,"targets":[]})", out));
}

} // namespace
