#include "core/snapshot/secure_dir.h"

#include "util/path_util.h"

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
    // TOKEN_USER はポインタ(SID*)メンバを含み alignof は 8。char バッファ流用の reinterpret_cast は
    // 規格上アラインメント未保証のため、最大基本アラインメントで確保される vector<unsigned char>
    // を使う。
    std::vector<unsigned char> buf(len);
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

// 既存・新規どちらの対象にも owner-only DACL を冪等に「強制」適用する（S2/S3/D4）。
// 事前に弱い権限で作成されていたディレクトリ／ACL付き作成に失敗して継承ACLで作られたディレクトリの
// 秘匿性を是正する。継承を断つため PROTECTED_DACL を指定する。成功時 true。
bool apply_owner_only_dacl(const std::wstring& wpath)
{
    PSECURITY_DESCRIPTOR sd = build_owner_only_sd();
    if (sd == nullptr)
    {
        return false;
    }
    BOOL dacl_present = FALSE;
    BOOL dacl_defaulted = FALSE;
    PACL dacl = nullptr;
    bool ok = false;
    if (::GetSecurityDescriptorDacl(sd, &dacl_present, &dacl, &dacl_defaulted) && dacl_present)
    {
        const DWORD rc =
            ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wpath.c_str()), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                    nullptr, nullptr, dacl, nullptr);
        ok = (rc == ERROR_SUCCESS);
    }
    ::LocalFree(sd);
    return ok;
}

} // namespace

bool create_secure_dir(std::string_view path)
{
    const fs::path target = pika::util::utf8_to_path(path);
    std::error_code ec;

    if (!fs::exists(target, ec))
    {
        // 親から順に作る。最上位の既存ディレクトリまで遡ってから降りる。
        std::vector<fs::path> chain;
        for (fs::path p = target; !p.empty(); p = p.parent_path())
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
        bool created = true;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            // it は utf8_to_path 由来でネイティブ wide を保持する。CP_ACP を経由する it->string()
            // ではなく UTF-8（path_to_utf8）→ to_wide(CP_UTF8) で正しいワイドパスにして
            // CreateDirectoryW へ渡す。
            if (!create_one(to_wide(pika::util::path_to_utf8(*it)), sd))
            {
                // ACL 付き作成に失敗したら継承 ACL での作成へフォールバック（保全優先。秘匿性は
                // 後段の DACL 強制適用で是正する）。
                if (!create_one(to_wide(pika::util::path_to_utf8(*it)), nullptr))
                {
                    created = false;
                    break;
                }
            }
        }
        if (sd != nullptr)
        {
            ::LocalFree(sd);
        }
        if (!created)
        {
            return false; // 作成自体に失敗（保全不能）
        }
    }

    // 既存（事前に弱権限で作られた可能性）・継承ACLフォールバックで作られたディレクトリの双方を
    // 是正するため、対象へ owner-only DACL
    // を冪等に強制適用する（S2/S3/D4。要件9.1）。失敗は秘匿性が
    // 担保できなかったシグナル（戻り値で伝える。データ作成自体はここまでで完了済み）。
    return apply_owner_only_dacl(to_wide(pika::util::path_to_utf8(target)));
}

} // namespace pika::core::snapshot
