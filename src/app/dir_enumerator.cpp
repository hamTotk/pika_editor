#include "app/dir_enumerator.h"

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

} // namespace pika::app
