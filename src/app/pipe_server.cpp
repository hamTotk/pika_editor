#include "app/pipe_server.h"

#include "core/ipc/ipc_message.h"
#include "core/ipc/pipe_security.h"

#include <sddl.h>

#include <vector>

namespace pika::app
{

namespace
{

// 受信は最大数KBで打ち切る（要件3.2）。kMaxMessageBytes をバッファ上限にする。
constexpr DWORD kReadBufferBytes = static_cast<DWORD>(core::ipc::kMaxMessageBytes);

} // namespace

std::string current_user_sid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return std::string();
    }

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0)
    {
        CloseHandle(token);
        return std::string();
    }

    std::vector<unsigned char> buf(needed);
    std::string sid;
    if (GetTokenInformation(token, TokenUser, buf.data(), needed, &needed))
    {
        const auto* tu = reinterpret_cast<const TOKEN_USER*>(buf.data());
        LPSTR str = nullptr;
        if (ConvertSidToStringSidA(tu->User.Sid, &str) && str != nullptr)
        {
            sid.assign(str);
            LocalFree(str);
        }
    }
    CloseHandle(token);
    return sid;
}

PipeServer::~PipeServer()
{
    stop();
}

AcquireResult PipeServer::try_acquire(const std::string& pipe_name, const std::string& owner_sddl)
{
    pipe_name_ = pipe_name;
    owner_sddl_ = owner_sddl;

    // owner-only DACL（make_owner_only_sddl の SDDL）から SECURITY_ATTRIBUTES を組み立てる。
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const bool sd_ok = ConvertStringSecurityDescriptorToSecurityDescriptorA(
        owner_sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);

    // fail-closed（要件3.2）: owner-only DACL の SECURITY_DESCRIPTOR を構築できないときは、
    // nullptr（SECURITY_ATTRIBUTES 不指定＝既定 DACL）へフォールバックして既定 DACL のパイプを
    // 作る経路を撤廃する。owner-less パイプを公開すると『作成者ユーザーのみ許可する owner-only
    // DACL』が黙って外れる（fail-open）ため、パイプを一切作らず InsecureNotCreated を返す。
    // 呼び出し側（main_gui）はこれをスタンドアロン縮退として扱い、IPC を張らずに自分で開く。
    if (!sd_ok)
    {
        // ConvertString... が失敗した場合 sd は未確定なので LocalFree しない（失敗時は確保なし）。
        acquired_ = false;
        return AcquireResult::InsecureNotCreated;
    }
    sa.lpSecurityDescriptor = sd;

    const core::ipc::PipeOpenPolicy policy = core::ipc::default_policy();
    DWORD open_mode = PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED;
    DWORD pipe_mode = PIPE_TYPE_MESSAGE | PIPE_WAIT;
    if (policy.message_mode)
    {
        pipe_mode |= PIPE_READMODE_MESSAGE;
    }
    if (policy.reject_remote_clients)
    {
        pipe_mode |= PIPE_REJECT_REMOTE_CLIENTS;
    }
    const DWORD in_buffer = static_cast<DWORD>(policy.in_buffer_bytes);

    // FILE_FLAG_FIRST_PIPE_INSTANCE: 最初の 1 プロセスだけが作成に成功する（原子的ロック）。
    // 既存インスタンスがあれば ERROR_ACCESS_DENIED で失敗＝このプロセスはクライアント。
    // owner-only DACL の SECURITY_DESCRIPTOR を必ず指定する（既定 DACL へは決して落ちない）。
    pipe_ = CreateNamedPipeA(pipe_name.c_str(), open_mode, pipe_mode,
                             /*nMaxInstances*/ 1, /*nOutBufferSize*/ 0, in_buffer,
                             /*nDefaultTimeOut*/ 0, &sa);

    LocalFree(sd);

    acquired_ = (pipe_ != INVALID_HANDLE_VALUE);
    return acquired_ ? AcquireResult::Server : AcquireResult::Client;
}

void PipeServer::start_listening(OnRequestLine on_line)
{
    if (!acquired_ || listening_)
    {
        return;
    }
    on_line_ = std::move(on_line);
    stop_event_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (stop_event_ == nullptr)
    {
        return;
    }
    listening_ = true;
    thread_ = CreateThread(
        nullptr, 0,
        [](LPVOID self) -> DWORD {
            static_cast<PipeServer*>(self)->listen_loop();
            return 0;
        },
        this, 0, nullptr);
}

void PipeServer::listen_loop()
{
    // 1 接続ずつ受け、1 メッセージ（1 行 JSON）を読み、コールバックへ渡す。重い処理はしない
    // （UI スレッドへのマーシャリングは on_line_ 実装が CallAfter で行う。design.md 4章）。
    std::vector<char> buf(kReadBufferBytes);
    while (true)
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (ov.hEvent == nullptr)
        {
            break;
        }

        BOOL connected = ConnectNamedPipe(pipe_, &ov);
        DWORD err = GetLastError();
        if (!connected && err == ERROR_IO_PENDING)
        {
            HANDLE waits[2] = {ov.hEvent, stop_event_};
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 1)
            {
                CloseHandle(ov.hEvent);
                break; // 停止合図。
            }
        }
        else if (!connected && err != ERROR_PIPE_CONNECTED)
        {
            CloseHandle(ov.hEvent);
            // 停止合図が立っていれば抜ける。そうでなければ再試行。
            if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0)
            {
                break;
            }
            continue;
        }

        // 1 メッセージを読む（最大 kReadBufferBytes で打ち切り＝要件3.2）。
        DWORD read = 0;
        OVERLAPPED rov{};
        rov.hEvent = ov.hEvent;
        ResetEvent(rov.hEvent);
        BOOL ok = ReadFile(pipe_, buf.data(), kReadBufferBytes, &read, &rov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
        {
            HANDLE waits[2] = {rov.hEvent, stop_event_};
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 1)
            {
                CloseHandle(ov.hEvent);
                break;
            }
            ok = GetOverlappedResult(pipe_, &rov, &read, FALSE);
        }

        if (ok && read > 0 && on_line_)
        {
            on_line_(std::string(buf.data(), read));
        }

        DisconnectNamedPipe(pipe_);
        CloseHandle(ov.hEvent);
    }
}

void PipeServer::stop()
{
    if (stop_event_ != nullptr)
    {
        SetEvent(stop_event_);
    }
    if (thread_ != nullptr)
    {
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stop_event_ != nullptr)
    {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
    if (pipe_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    listening_ = false;
    acquired_ = false;
}

bool send_to_server(const std::string& pipe_name, const std::string& transfer_json)
{
    // サーバーが受信ループに入る一瞬は ERROR_PIPE_BUSY になりうる。短時間リトライする。
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        HANDLE h =
            CreateFileA(pipe_name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
            DWORD written = 0;
            // kMaxMessageBytes を超える転送は送らない（信頼境界。受信側も打ち切る）。
            const DWORD len = static_cast<DWORD>(transfer_json.size() > core::ipc::kMaxMessageBytes
                                                     ? core::ipc::kMaxMessageBytes
                                                     : transfer_json.size());
            const BOOL ok = WriteFile(h, transfer_json.data(), len, &written, nullptr);
            CloseHandle(h);
            return ok && written == len;
        }
        if (GetLastError() != ERROR_PIPE_BUSY)
        {
            return false; // サーバー不在等。
        }
        WaitNamedPipeA(pipe_name.c_str(), 100);
    }
    return false;
}

} // namespace pika::app
