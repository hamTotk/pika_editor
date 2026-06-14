#include "core/snapshot/index_io.h"

#include "core/snapshot/json_lite.h"
#include "util/atomic_file.h"

#include <filesystem>
#include <system_error>

namespace pika::core::snapshot
{

namespace fs = std::filesystem;
using pika::util::ErrorCode;
using pika::util::Result;

std::string serialize_index(const SnapshotIndex& index)
{
    json::Value root = json::Value::object();
    root.set("version", json::Value::integer(index.version));

    json::Value entries = json::Value::array();
    for (const auto& e : index.entries)
    {
        json::Value je = json::Value::object();
        je.set("relPath", json::Value::str(e.rel_path));
        je.set("baselineHash", json::Value::str(e.baseline_hash));
        je.set("baselineObject", json::Value::str(e.baseline_object));
        je.set("baselineMtime", json::Value::integer(e.baseline_mtime));
        je.set("baselineSize", json::Value::integer(static_cast<std::int64_t>(e.baseline_size)));
        je.set("unread", json::Value::boolean(e.unread));

        json::Value stash = json::Value::array();
        for (const auto& s : e.stash)
        {
            json::Value js = json::Value::object();
            js.set("hash", json::Value::str(s.hash));
            js.set("time", json::Value::integer(s.time));
            js.set("kind", json::Value::str(to_string(s.kind)));
            js.set("batchId", json::Value::str(s.batch_id));
            js.set("restored", json::Value::boolean(s.restored));
            stash.push_back(std::move(js));
        }
        je.set("stash", std::move(stash));
        entries.push_back(std::move(je));
    }
    root.set("entries", std::move(entries));
    return root.dump();
}

Result<SnapshotIndex> parse_index(std::string_view text)
{
    json::Value root;
    if (!json::parse(text, root) || !root.is_object())
    {
        return Result<SnapshotIndex>::err(ErrorCode::Io, "index.json のパースに失敗しました");
    }
    SnapshotIndex index;
    const json::Value* ver = root.get("version");
    if (ver == nullptr)
    {
        return Result<SnapshotIndex>::err(ErrorCode::Io, "index.json に version がありません");
    }
    index.version = static_cast<int>(ver->as_int(0));
    // 未知（新しい）version は読み込まず安全側に倒す（K2）。
    if (index.version > kIndexVersion)
    {
        return Result<SnapshotIndex>::err(ErrorCode::Unsupported,
                                          "index.json の version が新しすぎます");
    }

    const json::Value* entries = root.get("entries");
    if (entries != nullptr && entries->is_array())
    {
        for (const auto& je : entries->items())
        {
            if (!je.is_object())
            {
                continue;
            }
            IndexEntry e;
            if (const auto* p = je.get("relPath"))
            {
                e.rel_path = p->as_string();
            }
            if (const auto* p = je.get("baselineHash"))
            {
                e.baseline_hash = p->as_string();
            }
            if (const auto* p = je.get("baselineObject"))
            {
                e.baseline_object = p->as_string();
            }
            if (const auto* p = je.get("baselineMtime"))
            {
                e.baseline_mtime = p->as_int();
            }
            if (const auto* p = je.get("baselineSize"))
            {
                e.baseline_size = static_cast<std::uint64_t>(p->as_int());
            }
            if (const auto* p = je.get("unread"))
            {
                e.unread = p->as_bool();
            }
            if (const auto* st = je.get("stash"); st != nullptr && st->is_array())
            {
                for (const auto& js : st->items())
                {
                    if (!js.is_object())
                    {
                        continue;
                    }
                    StashEntry s;
                    if (const auto* p = js.get("hash"))
                    {
                        s.hash = p->as_string();
                    }
                    if (const auto* p = js.get("time"))
                    {
                        s.time = p->as_int();
                    }
                    if (const auto* p = js.get("kind"))
                    {
                        parse_stash_kind(p->as_string(), s.kind);
                    }
                    if (const auto* p = js.get("batchId"))
                    {
                        s.batch_id = p->as_string();
                    }
                    if (const auto* p = js.get("restored"))
                    {
                        s.restored = p->as_bool();
                    }
                    e.stash.push_back(std::move(s));
                }
            }
            index.entries.push_back(std::move(e));
        }
    }
    return Result<SnapshotIndex>::ok(std::move(index));
}

Result<SnapshotIndex> load_index(std::string_view index_path)
{
    std::error_code ec;
    if (!fs::exists(fs::path(index_path), ec))
    {
        SnapshotIndex fresh; // 初回オープン：空の現行 version
        fresh.version = kIndexVersion;
        return Result<SnapshotIndex>::ok(std::move(fresh));
    }
    auto bytes = pika::util::read_all(index_path);
    if (bytes.is_err())
    {
        return Result<SnapshotIndex>::err(bytes.error());
    }
    return parse_index(bytes.value());
}

Result<void> save_index(std::string_view index_path, const SnapshotIndex& index)
{
    std::error_code ec;
    fs::create_directories(fs::path(index_path).parent_path(), ec);
    return pika::util::write_atomic(index_path, serialize_index(index));
}

} // namespace pika::core::snapshot
