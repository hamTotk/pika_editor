#include "core/ipc/pipe_security.h"

namespace pika::core::ipc
{

std::string make_pipe_name(const std::string& user_sid)
{
    // 形式は `\\.\pipe\pika-<SID>`（要件3.2）。SID をそのまま付与しユーザー別に分離する。
    return "\\\\.\\pipe\\pika-" + user_sid;
}

std::string make_owner_only_sddl(const std::string& user_sid)
{
    // D:(A;;GA;;;<SID>) = DACL に「<SID> へ GENERIC_ALL を Allow」する ACE を 1 つだけ置く。
    // 他ユーザーには ACE を与えない（DACL に該当 ACE が無い＝アクセス不可）。
    return "D:(A;;GA;;;" + user_sid + ")";
}

PipeOpenPolicy default_policy()
{
    // 既定で遠隔拒否・メッセージ境界・受信上限 8KB（要件3.2「最大数KB」と整合）。
    PipeOpenPolicy p;
    p.reject_remote_clients = true;
    p.message_mode = true;
    p.in_buffer_bytes = 8 * 1024;
    return p;
}

} // namespace pika::core::ipc
