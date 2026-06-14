// core/ipc 名前付きパイプ保護方針の検証（sprint7 should「名前付きパイプ \\.\pipe\pika-<SID> の
// DACL(作成者のみ)＋PIPE_REJECT_REMOTE_CLIENTS の設定方針」）。パイプ名・SDDL・既定方針フラグの
// 組み立てを観測する（Win32 呼び出しはプラットフォーム層が行う。ここは方針の決定論検証）。
#include "core/ipc/pipe_security.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::ipc::default_policy;
using pika::core::ipc::make_owner_only_sddl;
using pika::core::ipc::make_pipe_name;

TEST(PipeSecurity, PipeNameIncludesSid)
{
    const std::string sid = "S-1-5-21-1-2-3-1001";
    EXPECT_EQ(make_pipe_name(sid), "\\\\.\\pipe\\pika-" + sid);
}

TEST(PipeSecurity, SddlGrantsOnlyOwnerSid)
{
    const std::string sid = "S-1-5-21-1-2-3-1001";
    const std::string sddl = make_owner_only_sddl(sid);
    // D:(A;;GA;;;<SID>) = その SID に GENERIC_ALL を Allow する ACE 1 つだけ。
    EXPECT_EQ(sddl, "D:(A;;GA;;;" + sid + ")");
    // 他者向けの広域 ACE（Everyone=WD 等）を含まないこと。
    EXPECT_EQ(sddl.find("WD"), std::string::npos); // Everyone
    EXPECT_EQ(sddl.find("AN"), std::string::npos); // Anonymous
}

TEST(PipeSecurity, DefaultPolicyRejectsRemoteClients)
{
    auto p = default_policy();
    EXPECT_TRUE(p.reject_remote_clients);
    EXPECT_TRUE(p.message_mode);
    EXPECT_GT(p.in_buffer_bytes, 0u);
    EXPECT_LE(p.in_buffer_bytes, 64u * 1024u); // 「数KB」レンジ
}

} // namespace
