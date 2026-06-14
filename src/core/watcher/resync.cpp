#include "core/watcher/resync.h"

#include "core/watcher/fs_probe.h"
#include "util/path_util.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace pika::core::watcher
{

namespace fs = std::filesystem;

bool is_excluded_dir(std::string_view name)
{
    // 既定除外（design.md・要件4章）。完全一致のディレクトリ名で枝刈りする。
    return name == ".git" || name == "node_modules";
}

namespace
{

// root 相対パスを '/' 区切りの UTF-8 で返す（baseline のキー表記＝'/' 区切り UTF-8 に合わせる）。
// generic_string() は Windows で CP_ACP のナロー文字列になり、非ASCIIパスでは baseline キーと
// 不一致（毎回 Created/Removed・Modified 検知崩壊）になるため generic_u8string で UTF-8 を保つ。
std::string rel_of(const fs::path& root, const fs::path& p)
{
    std::error_code ec;
    fs::path rel = fs::relative(p, root, ec);
    const std::u8string u8 = ec ? p.generic_u8string() : rel.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 監視時刻と同じ意味（Win32 FILETIME・size）で stat する。
// last_write_time の epoch 差を避けるため fs_probe::probe を使う。
struct Enumerated
{
    std::string rel;
    FileStat st;
};

void enumerate(const fs::path& root, std::vector<Enumerated>& out)
{
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec)
    {
        return; // 列挙不能（権限等）は空＝呼び出し側で安全側に倒す
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        const fs::path& p = it->path();
        std::error_code dec;
        if (it->is_directory(dec))
        {
            if (is_excluded_dir(pika::util::path_to_utf8(p.filename())))
            {
                it.disable_recursion_pending(); // .git/node_modules 配下へ降りない
            }
            continue;
        }
        if (!it->is_regular_file(dec))
        {
            continue; // シンボリックリンク先・特殊ファイルは内容ファイルとして扱わない
        }
        Enumerated e;
        e.rel = rel_of(root, p);
        e.st = probe(pika::util::path_to_utf8(p));
        if (e.st.exists)
        {
            out.push_back(std::move(e));
        }
    }
}

} // namespace

std::vector<FsEvent> resync(std::string_view root, const BaselineMap& baseline)
{
    const fs::path root_path = pika::util::utf8_to_path(root);

    std::vector<Enumerated> disk;
    enumerate(root_path, disk);

    std::vector<FsEvent> out;

    // 実在ファイルを baseline と突き合わせる（新規 / 変更）。
    // 実在集合を作りながら、後で baseline 側の消失（削除）を拾う。
    std::vector<std::string> present;
    present.reserve(disk.size());
    for (const auto& e : disk)
    {
        present.push_back(e.rel);
        auto bit = baseline.find(e.rel);
        if (bit == baseline.end())
        {
            // baseline に無い → 新規。
            FsEvent ev;
            ev.kind = FsEventKind::Created;
            ev.path = e.rel;
            out.push_back(std::move(ev));
            continue;
        }
        const BaselineEntry& base = bit->second;
        // プレスクリーン: mtime+size が一致なら無変化（ハッシュを計算しない）。
        if (e.st.size == base.size && e.st.mtime_ns == base.mtime_ns)
        {
            continue;
        }
        // 不一致のみハッシュ比較（改行のみ差・touch だけの mtime 変化を内容変更と誤らない）。
        auto h = content_hash_lf(
            e.rel.empty() ? pika::util::path_to_utf8(root_path)
                          : pika::util::path_to_utf8(root_path / pika::util::utf8_to_path(e.rel)));
        if (h.is_err())
        {
            // 内容ハッシュが読めない（一時ロック・I/O 障害等）。mtime/size は既にベースラインと
            // 相違している区間なので、ここで無変化扱いにすると再同期で実変更を取りこぼす
            // （設計原則1「データを失わない」に反する）。ハッシュを捏造して比較するのではなく、
            // 取りこぼし回避を優先して Modified を明示的に発行する（内容が実は同一だった場合の
            // 確認プロンプトは、レビュー伴走ツールとして許容する安全側のコスト）。
            FsEvent ev;
            ev.kind = FsEventKind::Modified;
            ev.path = e.rel;
            out.push_back(std::move(ev));
            continue;
        }
        if (h.value() != base.hash_lf)
        {
            FsEvent ev;
            ev.kind = FsEventKind::Modified;
            ev.path = e.rel;
            out.push_back(std::move(ev));
        }
    }

    // baseline にあって実在しないものは削除。
    std::vector<std::string> present_sorted = present;
    std::sort(present_sorted.begin(), present_sorted.end());
    for (const auto& [rel, base] : baseline)
    {
        (void)base;
        if (!std::binary_search(present_sorted.begin(), present_sorted.end(), rel))
        {
            FsEvent ev;
            ev.kind = FsEventKind::Removed;
            ev.path = rel;
            out.push_back(std::move(ev));
        }
    }

    // relPath 昇順で決定論化する。
    std::sort(out.begin(), out.end(),
              [](const FsEvent& a, const FsEvent& b) { return a.path < b.path; });
    return out;
}

} // namespace pika::core::watcher
