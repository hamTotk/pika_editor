#include "controller/data_root.h"

namespace pika::controller
{

namespace
{

// 末尾のディレクトリ区切り（'/' と '\\'）をすべて畳む。区切りは後段で '\\' に統一する。
std::string strip_trailing_separators(std::string s)
{
    while (!s.empty() && (s.back() == '/' || s.back() == '\\'))
    {
        s.pop_back();
    }
    return s;
}

// '/' を Windows のバックスラッシュ '\\' に統一する（混在入力の正規化）。
std::string to_backslash(std::string s)
{
    for (char& c : s)
    {
        if (c == '/')
        {
            c = '\\';
        }
    }
    return s;
}

// 親ディレクトリと子セグメントを '\\' で連結する（親の末尾区切りは既に畳んである前提）。
std::string join(const std::string& parent, const std::string& child)
{
    return parent + "\\" + child;
}

} // namespace

DataRoot resolve_data_root(const DataRootProbe& probe)
{
    DataRoot out;

    if (probe.portable_marker_present)
    {
        // ポータブル版: exe 隣の ./pika-data/（要件13章）。exe_dir が無ければ確定できない。
        std::string base = strip_trailing_separators(to_backslash(probe.exe_dir));
        if (base.empty())
        {
            return out; // resolved=false
        }
        out.resolved = true;
        out.kind = DataRootKind::Portable;
        out.path = join(base, "pika-data");
        return out;
    }

    // 通常版: %LOCALAPPDATA%\pika\（design.md 7章 K1）。取得不能なら確定できない。
    std::string base = strip_trailing_separators(to_backslash(probe.local_app_data));
    if (base.empty())
    {
        return out; // resolved=false
    }
    out.resolved = true;
    out.kind = DataRootKind::LocalAppData;
    out.path = join(base, "pika");
    return out;
}

} // namespace pika::controller
