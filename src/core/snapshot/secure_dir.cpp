#include "core/snapshot/secure_dir.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

namespace pika::core::snapshot
{

namespace fs = std::filesystem;

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

// 現在のユーザー（プロセストークンの所有者 SID）のみにフルアクセスを与える
// SECURITY_DESCRIPTOR を SDDL 文字列から組み立てる。失敗時は nullptr を返す。
// 返り値は ::LocalFree で解放する（呼び出し側が管理する）。
PSECURITY_DESCRIPTOR build_owner_only_sd()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return nullptr;
    }
    DWORD len = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::string buf(len, '\0');
    if (len == 0 || !::GetTokenInformation(token, TokenUser, buf.data(), len, &len))
    {
        ::CloseHandle(token);
        return nullptr;
    }
    ::CloseHandle(token);

    auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
    LPWSTR sid_str = nullptr;
    if (!::ConvertSidToStringSidW(tu->User.Sid, &sid_str))
    {
        return nullptr;
    }

    // DACL を protected（P）にして継承を断ち、所有者 SID にのみ FullAccess（FA）を与える。
    std::wstring sddl = L"D:P(A;OICI;FA;;;";
    sddl += sid_str;
    sddl += L")";
    ::LocalFree(sid_str);

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd,
                                                                nullptr))
    {
        return nullptr;
    }
    return sd;
}

bool create_one(const std::wstring& wpath, PSECURITY_DESCRIPTOR sd)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = sd;
    if (::CreateDirectoryW(wpath.c_str(), sd != nullptr ? &sa : nullptr))
    {
        return true;
    }
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

} // namespace

bool create_secure_dir(std::string_view path)
{
    std::error_code ec;
    if (fs::exists(fs::path(path), ec))
    {
        return true;
    }

    // 親から順に作る。最上位の既存ディレクトリまで遡ってから降りる。
    std::vector<fs::path> chain;
    for (fs::path p = fs::path(path); !p.empty(); p = p.parent_path())
    {
        if (fs::exists(p, ec))
        {
            break;
        }
        chain.push_back(p);
        if (p == p.root_path())
        {
            break;
        }
    }

    PSECURITY_DESCRIPTOR sd = build_owner_only_sd();
    bool ok = true;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        if (!create_one(to_wide(it->string()), sd))
        {
            // ACL 付き作成に失敗したら、継承 ACL での作成へフォールバックする（保全優先）。
            if (!create_one(to_wide(it->string()), nullptr))
            {
                ok = false;
                break;
            }
        }
    }
    if (sd != nullptr)
    {
        ::LocalFree(sd);
    }
    return ok;
}

} // namespace pika::core::snapshot
