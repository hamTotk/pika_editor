#include "core/ipc/path_normalizer.h"

#include <vector>

namespace pika::core::ipc
{

namespace
{

bool is_sep(char c)
{
    return c == '/' || c == '\\';
}

bool is_drive_letter(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// path のルート接頭辞（`C:\` / `C:/` → "C:\\"、UNC `\\srv\share` → "\\\\srv\\share\\"）を
// 取り出す。残りの相対部分の開始位置を rest_pos に返す。ルートが無ければ root は空・rest_pos=0。
std::string take_root(const std::string& path, std::size_t& rest_pos)
{
    rest_pos = 0;
    if (path.size() >= 2 && is_sep(path[0]) && is_sep(path[1]))
    {
        // UNC: \\server\share をルートに含める（share まで取り込む）。
        std::string root = "\\\\";
        std::size_t i = 2;
        // server
        while (i < path.size() && !is_sep(path[i]))
        {
            root.push_back(path[i]);
            ++i;
        }
        if (i < path.size())
        {
            root.push_back('\\');
            ++i;
        }
        // share
        while (i < path.size() && !is_sep(path[i]))
        {
            root.push_back(path[i]);
            ++i;
        }
        root.push_back('\\');
        // 続く区切りはスキップ
        while (i < path.size() && is_sep(path[i]))
        {
            ++i;
        }
        rest_pos = i;
        return root;
    }
    if (path.size() >= 3 && is_drive_letter(path[0]) && path[1] == ':' && is_sep(path[2]))
    {
        std::string root;
        root.push_back(path[0]);
        root.push_back(':');
        root.push_back('\\');
        std::size_t i = 3;
        while (i < path.size() && is_sep(path[i]))
        {
            ++i;
        }
        rest_pos = i;
        return root;
    }
    return std::string();
}

// 相対部分（区切りで分割した名前列）を `.`/`..` を解決しながら components に積む。
void apply_segments(const std::string& rel, std::vector<std::string>& components)
{
    std::string seg;
    auto flush = [&]() {
        if (seg.empty() || seg == ".")
        {
            seg.clear();
            return;
        }
        if (seg == "..")
        {
            if (!components.empty())
            {
                components.pop_back();
            }
            // ルートを超える `..` はルートで止める（pop しない）。
            seg.clear();
            return;
        }
        components.push_back(seg);
        seg.clear();
    };
    for (char c : rel)
    {
        if (is_sep(c))
        {
            flush();
        }
        else
        {
            seg.push_back(c);
        }
    }
    flush();
}

std::string join(const std::string& root, const std::vector<std::string>& components)
{
    std::string out = root;
    bool first = true;
    for (const std::string& c : components)
    {
        if (!first || (!root.empty() && root.back() != '\\'))
        {
            // root が区切りで終わっていればそのまま、そうでなければ区切りを挟む。
            if (!out.empty() && out.back() != '\\')
            {
                out.push_back('\\');
            }
        }
        out += c;
        first = false;
    }
    return out;
}

} // namespace

std::string normalize_to_absolute(const std::string& path, const std::string& cwd)
{
    std::size_t rest_pos = 0;
    std::string root = take_root(path, rest_pos);

    if (!root.empty())
    {
        // path が既に絶対。ルート以降を `.`/`..` 解決する。
        std::vector<std::string> components;
        apply_segments(path.substr(rest_pos), components);
        return join(root, components);
    }

    // ここから path は非絶対。cwd（絶対前提）のルートを土台にする。
    std::size_t cwd_rest = 0;
    std::string cwd_root = take_root(cwd, cwd_rest);
    std::vector<std::string> components;
    if (!cwd_root.empty())
    {
        apply_segments(cwd.substr(cwd_rest), components);
    }

    // ドライブ相対 `C:foo`（ドライブレター＋コロンだが区切りなし）。
    // cwd と同じドライブのときのみ cwd 配下に解決し、違えば指定ドライブのルート直下に置く。
    if (path.size() >= 2 && is_drive_letter(path[0]) && path[1] == ':')
    {
        std::string drive_root;
        drive_root.push_back(path[0]);
        drive_root.push_back(':');
        drive_root.push_back('\\');
        const bool same_drive =
            cwd_root.size() >= 2 && (cwd_root[0] | 0x20) == (path[0] | 0x20) && cwd_root[1] == ':';
        if (same_drive)
        {
            apply_segments(path.substr(2), components);
            return join(cwd_root, components);
        }
        std::vector<std::string> only;
        apply_segments(path.substr(2), only);
        return join(drive_root, only);
    }

    // 純粋な相対パス: cwd 配下に連結する。
    apply_segments(path, components);
    return join(cwd_root, components);
}

} // namespace pika::core::ipc
