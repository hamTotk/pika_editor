#include "core/snapshot/snapshot_types.h"

namespace pika::core::snapshot
{

const char* to_string(StashKind kind) noexcept
{
    switch (kind)
    {
    case StashKind::Conflict:
        return "conflict";
    case StashKind::Incoming:
        return "incoming";
    case StashKind::Rollback:
        return "rollback";
    case StashKind::BaselineReplace:
        return "baseline-replace";
    }
    return "conflict";
}

bool parse_stash_kind(const std::string& s, StashKind& out) noexcept
{
    if (s == "conflict")
    {
        out = StashKind::Conflict;
        return true;
    }
    if (s == "incoming")
    {
        out = StashKind::Incoming;
        return true;
    }
    if (s == "rollback")
    {
        out = StashKind::Rollback;
        return true;
    }
    if (s == "baseline-replace")
    {
        out = StashKind::BaselineReplace;
        return true;
    }
    return false;
}

IndexEntry* SnapshotIndex::find(const std::string& rel_path) noexcept
{
    for (auto& e : entries)
    {
        if (e.rel_path == rel_path)
        {
            return &e;
        }
    }
    return nullptr;
}

const IndexEntry* SnapshotIndex::find(const std::string& rel_path) const noexcept
{
    for (const auto& e : entries)
    {
        if (e.rel_path == rel_path)
        {
            return &e;
        }
    }
    return nullptr;
}

} // namespace pika::core::snapshot
