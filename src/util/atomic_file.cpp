#include "util/atomic_file.h"

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pika::util
{

namespace
{

std::wstring to_wide(std::string_view utf8)
{
    if (utf8.empty())
    {
        return std::wstring{};
    }
    const int wlen =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), wlen);
    return w;
}

// path と同一ディレクトリに置く一時ファイル名を作る（別ボリューム rename を避ける）。
// 末尾に ".<pid>.<tick>.tmp" を付け、衝突しにくくする。
std::wstring make_temp_path(const std::wstring& final_path)
{
    std::wstring tmp = final_path;
    tmp += L".pika-";
    tmp += std::to_wstring(static_cast<unsigned long>(::GetCurrentProcessId()));
    tmp += L'-';
    tmp += std::to_wstring(static_cast<unsigned long long>(::GetTickCount64()));
    tmp += L".tmp";
    return tmp;
}

Result<void> write_all_to(const std::wstring& wpath, std::string_view bytes)
{
    HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return Result<void>::err(ErrorCode::Io, "一時ファイルの作成に失敗しました");
    }

    std::size_t written = 0;
    while (written < bytes.size())
    {
        const std::size_t remain = bytes.size() - written;
        const DWORD chunk = remain > 0x4000'0000u ? 0x4000'0000u : static_cast<DWORD>(remain);
        DWORD wrote = 0;
        if (!::WriteFile(h, bytes.data() + written, chunk, &wrote, nullptr))
        {
            ::CloseHandle(h);
            return Result<void>::err(ErrorCode::Io, "一時ファイルへの書き込みに失敗しました");
        }
        written += wrote;
    }

    // メタデータ・データをディスクへ落としてから rename する（クラッシュ耐性。要件12.1）。
    // フラッシュ失敗時はディスク確定が保証されないため、未確定の tmp を最終パスへ
    // 昇格させてはならない（rename 前にここで打ち切り tmp を削除する。データを失わない原則）。
    if (!::FlushFileBuffers(h))
    {
        ::CloseHandle(h);
        ::DeleteFileW(wpath.c_str());
        return Result<void>::err(ErrorCode::Io, "一時ファイルのフラッシュに失敗しました");
    }
    ::CloseHandle(h);
    return Result<void>::ok();
}

} // namespace

Result<void> write_atomic(std::string_view path, std::string_view bytes)
{
    if (path.empty())
    {
        return Result<void>::err(ErrorCode::InvalidArgument, "パスが空です");
    }
    const std::wstring wfinal = to_wide(path);
    const std::wstring wtmp = make_temp_path(wfinal);

    auto wrote = write_all_to(wtmp, bytes);
    if (wrote.is_err())
    {
        ::DeleteFileW(wtmp.c_str());
        return wrote;
    }

    // 最終パスへアトミック置換する。
    // 既存ありなら ReplaceFile（属性/ACL を維持・アトミック）。無ければ MoveFileEx で改名する。
    // 判定（GetFileAttributesW）と置換の間に他プロセスが最終パスを作りうる TOCTOU 窓があるため、
    // MoveFileEx には MOVEFILE_REPLACE_EXISTING を付け、窓内で既存が現れても改名が失敗しないよう
    // にする（既存上書きもアトミック。偽陰性での保存失敗を避ける）。
    const DWORD attrs = ::GetFileAttributesW(wfinal.c_str());
    const bool final_exists = attrs != INVALID_FILE_ATTRIBUTES;
    if (final_exists)
    {
        if (!::ReplaceFileW(wfinal.c_str(), wtmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS,
                            nullptr, nullptr))
        {
            ::DeleteFileW(wtmp.c_str());
            return Result<void>::err(ErrorCode::Io, "アトミック置換に失敗しました");
        }
    }
    else
    {
        if (!::MoveFileExW(wtmp.c_str(), wfinal.c_str(),
                           MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING))
        {
            ::DeleteFileW(wtmp.c_str());
            return Result<void>::err(ErrorCode::Io, "一時ファイルの改名に失敗しました");
        }
    }
    return Result<void>::ok();
}

Result<std::string> read_all(std::string_view path)
{
    if (path.empty())
    {
        return Result<std::string>::err(ErrorCode::InvalidArgument, "パスが空です");
    }
    const std::wstring wpath = to_wide(path);
    HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
        {
            return Result<std::string>::err(ErrorCode::NotFound, "ファイルが存在しません");
        }
        return Result<std::string>::err(ErrorCode::Io, "ファイルを開けませんでした");
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size))
    {
        ::CloseHandle(h);
        return Result<std::string>::err(ErrorCode::Io, "ファイルサイズの取得に失敗しました");
    }
    std::string out(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t read_total = 0;
    while (read_total < out.size())
    {
        const std::size_t remain = out.size() - read_total;
        const DWORD chunk = remain > 0x4000'0000u ? 0x4000'0000u : static_cast<DWORD>(remain);
        DWORD got = 0;
        if (!::ReadFile(h, out.data() + read_total, chunk, &got, nullptr))
        {
            ::CloseHandle(h);
            return Result<std::string>::err(ErrorCode::Io, "ファイルの読み込みに失敗しました");
        }
        if (got == 0)
        {
            break;
        }
        read_total += got;
    }
    ::CloseHandle(h);
    out.resize(read_total);
    return Result<std::string>::ok(std::move(out));
}

} // namespace pika::util
