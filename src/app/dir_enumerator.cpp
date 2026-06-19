#include "app/dir_enumerator.h"

#include "core/workspace/workspace_model.h"
#include "util/path_util.h"

#include <filesystem>
#include <system_error>

namespace pika::app
{

namespace fs = std::filesystem;

void list_directory(const std::string& root_abs, const std::string& dir_rel,
                    const OnDirListed& on_listed)
{
    std::vector<controller::RawListEntry> out;

    // 列挙対象の絶対パスを UTF-8→fs::path で組む（CP_ACP を介さない。util::utf8_to_path）。
    std::string target = root_abs;
    if (!dir_rel.empty())
    {
        target += "/";
        target += dir_rel;
    }
    const fs::path base = util::utf8_to_path(target);

    std::error_code ec;
    // symlink_status を使わず directory_iterator（既定でシンボリックリンクのターゲットを辿らない
    // ＝entry はリンク自体を指す）。is_directory
    // はリンク先を見るが、ここでは展開はせず種別のみ返す。
    fs::directory_iterator it(base, fs::directory_options::skip_permission_denied, ec);
    if (ec)
    {
        // 権限なし・消失等。空バッチで通知し、呼び出し側が縮退表示する（本 sprint は最小配線）。
        on_listed(dir_rel, std::move(out));
        return;
    }

    for (const fs::directory_entry& entry : it)
    {
        std::error_code dec;
        const bool is_dir = entry.is_directory(dec);
        controller::RawListEntry r;
        r.abs_path = util::path_to_utf8(entry.path());
        r.is_dir = !dec && is_dir;
        out.push_back(std::move(r));
    }

    on_listed(dir_rel, std::move(out));
}

std::vector<controller::RawListEntry> enumerate_tree(const std::string& root_abs,
                                                     const std::vector<std::string>& exclude,
                                                     std::size_t max_nodes, bool& capped)
{
    std::vector<controller::RawListEntry> out;
    capped = false;

    const fs::path base = util::utf8_to_path(root_abs);
    const std::size_t root_len = root_abs.size();

    std::error_code ec;
    // recursive_directory_iterator は既定でシンボリックリンクを辿らない（循環回避。要件4.1）。
    // 権限のないサブツリーはスキップして列挙を継続する。
    fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec);
    if (ec)
    {
        return out; // root 自体が開けない（権限なし・消失）。空で返し呼び出し側が縮退表示。
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec))
    {
        if (ec)
        {
            // 反復中のエラー（個別エントリの権限等）は無視して継続。
            ec.clear();
            continue;
        }
        const fs::directory_entry& entry = *it;
        std::error_code dec;
        const bool is_dir = entry.is_directory(dec);
        const bool dir = !dec && is_dir;

        // exclude 配下には降りない（.git/node_modules 等。性能の主因を入口で断つ）。
        // is_excluded はルート起点の相対パスで判定するため、abs から相対部分を取り出し '/'
        // に統一して渡す （path_to_utf8 は Windows で '\\' を含む。is_excluded は '/'
        // 区切り前提）。
        const std::string abs = util::path_to_utf8(entry.path());
        std::string rel;
        if (abs.size() > root_len + 1)
        {
            rel = abs.substr(root_len + 1); // 区切り 1 文字を飛ばす（'/' or '\\'）。
        }
        for (char& c : rel)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }
        if (dir && core::workspace::is_excluded(rel, exclude))
        {
            it.disable_recursion_pending(); // このディレクトリには降りない。
            continue;                       // ノード自体も木に含めない（normalize 側でも落ちる）。
        }

        if (out.size() >= max_nodes)
        {
            // 件数キャップ到達＝深さ無制限・件数暴走の最終ガード。打ち切る（呼び出し側がログを出す）。
            capped = true;
            break;
        }

        controller::RawListEntry r;
        r.abs_path = abs;
        r.is_dir = dir;
        out.push_back(std::move(r));
    }

    return out;
}

} // namespace pika::app
