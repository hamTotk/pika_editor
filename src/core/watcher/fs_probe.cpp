#include "core/watcher/fs_probe.h"

#include "util/atomic_file.h"
#include "util/hash.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pika::core::watcher
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

} // namespace

FileStat probe(std::string_view path)
{
    FileStat st;
    if (path.empty())
    {
        return st;
    }
    const std::wstring wpath = to_wide(path);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &data))
    {
        return st; // 取得不能は exists=false（安全側。監視は継続）
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        return st; // ディレクトリは内容ファイルとして扱わない
    }
    st.exists = true;
    st.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    st.mtime_ns = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                  data.ftLastWriteTime.dwLowDateTime;
    return st;
}

pika::util::Result<std::uint64_t> content_hash_lf(std::string_view path)
{
    auto bytes = pika::util::read_all(path);
    if (bytes.is_err())
    {
        return pika::util::Result<std::uint64_t>::err(bytes.error());
    }
    return pika::util::Result<std::uint64_t>::ok(pika::util::xxh3_64_lf(bytes.value()));
}

} // namespace pika::core::watcher
