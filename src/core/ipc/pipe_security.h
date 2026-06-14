// core/ipc: 名前付きパイプの命名と保護方針（信頼境界の設定）。
// 要件3.2「名前付きパイプ `\\.\pipe\pika-<ユーザーSID>` は作成者ユーザーのみ許可するDACLと
// `PIPE_REJECT_REMOTE_CLIENTS`（遠隔接続拒否）で保護する」/ design.md 95行。
//
// パイプ名の組み立てと、DACL を表す SDDL 文字列・CreateNamedPipe に渡すフラグの組み立てを
// 純ロジックとして切り出す（Win32 の CreateNamedPipe 呼び出し自体はプラットフォーム層が行うが、
// 「どの SID で、どの SDDL／フラグで保護するか」の方針はここで決定論的に組み立て
// gtest で検証する）。
#pragma once

#include <cstdint>
#include <string>

namespace pika::core::ipc
{

// ユーザー SID（例 "S-1-5-21-...-1001"）からパイプ名を組み立てる。
// 形式は `\\.\pipe\pika-<SID>`（要件3.2）。SID 単位でユーザー別に分離し、他ユーザーと衝突させない。
std::string make_pipe_name(const std::string& user_sid);

// 作成者ユーザーのみにフルアクセスを許す DACL の SDDL 文字列を組み立てる。
// `D:(A;;GA;;;<SID>)` = DACL に「<SID> へ GENERIC_ALL を Allow」する ACE を 1 つだけ置く。
// 他ユーザー・Everyone・匿名には ACE を与えない（明示拒否ではなく非付与で遮断）。
std::string make_owner_only_sddl(const std::string& user_sid);

// CreateNamedPipe の dwOpenMode に必ず立てるべきフラグ集合の方針。
// PIPE_REJECT_REMOTE_CLIENTS（遠隔接続拒否）を含むことが必須（要件3.2）。
// 実値（Win32 マクロ）はプラットフォーム層が解決するため、
// ここでは「何を要求するか」を bool で表す。
struct PipeOpenPolicy
{
    // PIPE_REJECT_REMOTE_CLIENTS（遠隔クライアントを拒否）。
    bool reject_remote_clients = true;
    // メッセージ境界での受信（1 リクエスト=1 メッセージ）。
    bool message_mode = true;
    // 受信バッファ上限（バイト）。要件3.2「最大数KB」と整合させる。
    std::uint32_t in_buffer_bytes = 8 * 1024;
};

// 既定の保護方針を返す（要件3.2 を満たす値で固定）。
PipeOpenPolicy default_policy();

} // namespace pika::core::ipc
