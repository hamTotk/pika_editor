#include "controller/dir_lister.h"

#include <cctype>
#include <set>

namespace pika::controller
{

namespace
{

char lower_ascii(char c) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// path の '\\' を '/' に統一し、末尾区切りを剥がした正規形を返す（大小は変えない）。
std::string normalize_separators(std::string_view path)
{
    std::string out;
    out.reserve(path.size());
    for (char c : path)
    {
        out.push_back(c == '\\' ? '/' : c);
    }
    while (!out.empty() && out.back() == '/')
    {
        out.pop_back();
    }
    return out;
}

// a が b の接頭辞か（大文字小文字無視）。Windows FS の非感性照合（要件3.2）。
bool starts_with_ci(std::string_view a, std::string_view b) noexcept
{
    if (a.size() < b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < b.size(); ++i)
    {
        if (lower_ascii(a[i]) != lower_ascii(b[i]))
        {
            return false;
        }
    }
    return true;
}

} // namespace

std::string to_workspace_rel_path(std::string_view root, std::string_view abs)
{
    const std::string nroot = normalize_separators(root);
    const std::string nabs = normalize_separators(abs);
    if (nroot.empty() || nabs.empty())
    {
        return std::string();
    }
    // abs が root と一致＝ルート自身（相対パスなし）。
    if (nabs.size() == nroot.size() && starts_with_ci(nabs, nroot))
    {
        return std::string();
    }
    // root 配下であること（root の直後がセグメント境界 '/' であること。"C:/ab" が "C:/abc" を
    // 配下と誤認しないよう、境界を厳密に見る）。
    if (!starts_with_ci(nabs, nroot) || nabs[nroot.size()] != '/')
    {
        return std::string();
    }
    return nabs.substr(nroot.size() + 1);
}

std::vector<core::workspace::Entry> normalize_entries(std::string_view root,
                                                      const std::vector<RawListEntry>& raw,
                                                      const std::vector<std::string>& exclude)
{
    std::vector<core::workspace::Entry> out;
    out.reserve(raw.size());
    std::set<std::string> seen; // 重複 rel_path の畳み込み（同一バッチ内）。
    for (const auto& r : raw)
    {
        const std::string rel = to_workspace_rel_path(root, r.abs_path);
        if (rel.empty())
        {
            continue; // root 外・ルート自身・空は捨てる。
        }
        if (core::workspace::is_excluded(rel, exclude))
        {
            continue; // 除外配下は木に含めない（逐次列挙もここで止められる）。
        }
        if (!seen.insert(rel).second)
        {
            continue; // 既出（先勝ち）。
        }
        core::workspace::Entry e;
        e.rel_path = rel;
        e.is_dir = r.is_dir;
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace pika::controller
