#include "controller/baseline_merge.h"

#include <stdexcept>
#include <string>

namespace pika::controller
{

namespace
{

// baselineHash（xxh3_64_lf_hex の16進文字列）を uint64 へ戻す。空/不正は ok=false。
// content_hash_lf が返す値と同一の LF 正規化 XXH3-64（hash.cpp:
// xxh3_64_lf_hex=to_hex16(xxh3_64_lf)）。
bool parse_hex(const std::string& s, std::uint64_t& out)
{
    if (s.empty())
    {
        return false;
    }
    try
    {
        std::size_t consumed = 0;
        const unsigned long long v = std::stoull(s, &consumed, 16);
        if (consumed != s.size())
        {
            return false; // 末尾に非16進が混ざる＝不正
        }
        out = static_cast<std::uint64_t>(v);
        return true;
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
    catch (const std::out_of_range&)
    {
        return false;
    }
}

} // namespace

core::watcher::BaselineMap merge_index_into_baseline(core::watcher::BaselineMap disk,
                                                     const core::snapshot::SnapshotIndex& index)
{
    for (const auto& entry : index.entries)
    {
        std::uint64_t hash = 0;
        if (!parse_hex(entry.baseline_hash, hash))
        {
            continue; // ハッシュ無し/不正は対象外（勝手に取り繕わない）
        }
        auto it = disk.find(entry.rel_path);
        if (it == disk.end())
        {
            continue; // 実在しない rel は足さない（resync が Removed を出す責務）
        }
        // 確認済みベースラインで上書き（確認時点の内容と比較させる）。mtime は秒精度のため
        // 現 mtime_ns と一致せずプレスクリーンを外れ、resync がハッシュ照合する＝意図どおり。
        it->second.size = entry.baseline_size;
        it->second.mtime_ns = static_cast<std::uint64_t>(entry.baseline_mtime);
        it->second.hash_lf = hash;
    }
    return disk;
}

} // namespace pika::controller
