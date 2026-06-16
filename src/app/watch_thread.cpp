#include "app/watch_thread.h"

#include "controller/watch_event_map.h"
#include "util/path_util.h"

#include <process.h>

#include <string>
#include <vector>

namespace pika::app
{

namespace
{

namespace wat = pika::core::watcher;

// UTF-16（ReadDirectoryChangesW が返す相対パス）→ UTF-8。Win32 API を介すため本ファイルに残す。
std::string utf16_to_utf8(const wchar_t* data, std::size_t len)
{
    if (len == 0)
    {
        return std::string();
    }
    const int n =
        WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
    {
        return std::string();
    }
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(len), out.data(), n, nullptr, nullptr);
    return out;
}

// 単調増加のミリ秒時刻（RawEvent.at に使う。WatcherCore のデバウンス/rename 窓と整合）。
wat::TimeMs now_ms()
{
    return static_cast<wat::TimeMs>(GetTickCount64());
}

} // namespace

unsigned __stdcall WatchThread::thread_entry(void* arg)
{
    static_cast<WatchThread*>(arg)->run();
    return 0;
}

WatchThread::~WatchThread()
{
    stop();
}

bool WatchThread::open_dir(const std::string& root_abs)
{
    const std::wstring wroot = pika::util::utf8_to_path(root_abs).wstring();
    dir_ = CreateFileW(wroot.c_str(), FILE_LIST_DIRECTORY,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    return dir_ != INVALID_HANDLE_VALUE;
}

void WatchThread::start(const std::string& root_abs, OnRawEvent on_raw, OnResyncNeeded on_resync,
                        DWORD poll_interval_ms)
{
    root_ = root_abs;
    on_raw_ = std::move(on_raw);
    on_resync_ = std::move(on_resync);
    poll_interval_ms_ = poll_interval_ms;

    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);    // 手動リセット
    resync_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); // 自動リセット（F5）

    // 監視ハンドルを開けるか試す。開けなければポーリングフォールバック（watching_=false）。
    watching_ = open_dir(root_abs);
    started_ = true;

    thread_ = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, &WatchThread::thread_entry, this, 0, nullptr));
}

void WatchThread::request_resync()
{
    // 次の機会に on_resync(Manual) を 1 回送る（F5。要件11.2）。
    if (resync_event_ != nullptr)
    {
        SetEvent(resync_event_);
    }
}

void WatchThread::run()
{
    // 監視: ReadDirectoryChangesW（OVERLAPPED）。ポーリング: 一定間隔で on_resync(Poll)。
    // どちらも stop_event_ / resync_event_ を待ち、停止・F5 に即応する。
    constexpr DWORD kBufBytes = 64 * 1024; // 監視バッファ既定64KB（design.md 5.2）
    std::vector<unsigned char> buffer(kBufBytes);

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                              FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                              FILE_NOTIFY_CHANGE_CREATION;

    auto issue_read = [&]() -> bool {
        ResetEvent(ov.hEvent);
        return ReadDirectoryChangesW(dir_, buffer.data(), kBufBytes, TRUE, kFilter, nullptr, &ov,
                                     nullptr) != 0;
    };

    if (watching_ && !issue_read())
    {
        // 監視開始に失敗したらポーリングへ転落する。
        watching_ = false;
    }

    for (;;)
    {
        HANDLE waits[3] = {stop_event_, resync_event_, ov.hEvent};
        const DWORD count = watching_ ? 3 : 2;
        const DWORD timeout = watching_ ? INFINITE : poll_interval_ms_;
        const DWORD r = WaitForMultipleObjects(count, waits, FALSE, timeout);

        if (r == WAIT_OBJECT_0)
        {
            break; // stop
        }
        if (r == WAIT_OBJECT_0 + 1)
        {
            // F5（手動再同期）。
            if (on_resync_)
            {
                on_resync_(ResyncReason::Manual);
            }
            continue;
        }
        if (!watching_ && r == WAIT_TIMEOUT)
        {
            // ポーリングフォールバック tick。
            if (on_resync_)
            {
                on_resync_(ResyncReason::Poll);
            }
            continue;
        }
        if (watching_ && r == WAIT_OBJECT_0 + 2)
        {
            DWORD bytes = 0;
            if (!GetOverlappedResult(dir_, &ov, &bytes, FALSE))
            {
                const DWORD err = GetLastError();
                if (err == ERROR_NOTIFY_ENUM_DIR)
                {
                    // バッファオーバーフロー。全再列挙→resync で取りこぼしを回復する。
                    if (on_resync_)
                    {
                        on_resync_(ResyncReason::Overflow);
                    }
                }
                if (!issue_read())
                {
                    watching_ = false;
                }
                continue;
            }
            if (bytes == 0)
            {
                // バッファ溢れで件数 0（ERROR_NOTIFY_ENUM_DIR 相当）。再同期で回復する。
                if (on_resync_)
                {
                    on_resync_(ResyncReason::Overflow);
                }
            }
            else
            {
                // FILE_NOTIFY_INFORMATION を 1 件ずつ走査して RawEvent へ正規化する。
                const wat::TimeMs at = now_ms();
                std::size_t offset = 0;
                for (;;)
                {
                    const auto* info =
                        reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
                    const std::string rel =
                        utf16_to_utf8(info->FileName, info->FileNameLength / sizeof(wchar_t));
                    const auto ev = controller::make_raw_event(info->Action, rel, at);
                    if (ev.has_value() && on_raw_)
                    {
                        on_raw_(ev.value()); // UI スレッドへマーシャリングするのは呼び出し側。
                    }
                    if (info->NextEntryOffset == 0)
                    {
                        break;
                    }
                    offset += info->NextEntryOffset;
                }
            }
            if (!issue_read())
            {
                watching_ = false;
            }
            continue;
        }
        // それ以外（WAIT_FAILED 等）はループを抜ける（安全側）。
        break;
    }

    if (ov.hEvent != nullptr)
    {
        CloseHandle(ov.hEvent);
    }
}

void WatchThread::stop()
{
    if (!started_)
    {
        return;
    }
    if (stop_event_ != nullptr)
    {
        SetEvent(stop_event_);
    }
    // OVERLAPPED の待機を確実に解くため I/O をキャンセルする。
    if (dir_ != INVALID_HANDLE_VALUE)
    {
        CancelIoEx(dir_, nullptr);
    }
    if (thread_ != nullptr)
    {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (dir_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(dir_);
        dir_ = INVALID_HANDLE_VALUE;
    }
    if (stop_event_ != nullptr)
    {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
    if (resync_event_ != nullptr)
    {
        CloseHandle(resync_event_);
        resync_event_ = nullptr;
    }
    started_ = false;
    watching_ = false;
}

} // namespace pika::app
