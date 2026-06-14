#include "core/snapshot/object_store.h"

#include "core/snapshot/compression.h"
#include "core/snapshot/json_lite.h"
#include "util/atomic_file.h"
#include "util/hash.h"
#include "util/path_util.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace pika::core::snapshot
{

namespace fs = std::filesystem;
using pika::util::ErrorCode;
using pika::util::Result;

namespace
{

// サイドカーのファイル名サフィックス。object 名 + これ で 1 退避メタになる。
constexpr const char* kMetaSuffix = ".meta";

} // namespace

ObjectStore::ObjectStore(std::string objects_dir) : dir_(std::move(objects_dir)) {}

bool ObjectStore::is_valid_hash(const std::string& hash) noexcept
{
    // XXH3-64 hex は厳密に 16 桁の小文字 16 進数。これ以外（区切り文字・".."・空・長さ違い）は
    // index.json/サイドカー由来でもパス合成に通さない（objects フォルダ外への到達を遮断する）。
    if (hash.size() != 16)
    {
        return false;
    }
    for (const char c : hash)
    {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

std::string ObjectStore::object_path(const std::string& hash) const
{
    return pika::util::path_to_utf8(pika::util::utf8_to_path(dir_) / hash);
}

std::string ObjectStore::stash_discriminator(const std::string& rel_path, StashKind kind,
                                             std::int64_t time, const std::string& batch_id)
{
    // 退避を一意に識別するキー（区切りに非表示文字 US=0x1F
    // を使い、各フィールドの結合衝突を避ける）。
    std::string key = rel_path;
    key.push_back('\x1f');
    key += to_string(kind);
    key.push_back('\x1f');
    key += std::to_string(time);
    key.push_back('\x1f');
    key += batch_id;
    return pika::util::xxh3_64_hex(key);
}

std::string ObjectStore::stash_meta_path(const std::string& hash, const std::string& disc) const
{
    return pika::util::path_to_utf8(pika::util::utf8_to_path(dir_) /
                                    (hash + "." + disc + kMetaSuffix));
}

Result<std::string> ObjectStore::put(std::string_view content)
{
    const std::string hash = pika::util::xxh3_64_hex(content);
    if (contains(hash))
    {
        return Result<std::string>::ok(hash); // 重複排除：既存なら書かない
    }
    std::error_code ec;
    fs::create_directories(pika::util::utf8_to_path(dir_), ec); // 遅延作成（初回 put 時）
    auto compressed = compress(content);
    if (compressed.is_err())
    {
        return Result<std::string>::err(compressed.error());
    }
    auto wrote = pika::util::write_atomic(object_path(hash), compressed.value());
    if (wrote.is_err())
    {
        return Result<std::string>::err(wrote.error());
    }
    return Result<std::string>::ok(hash);
}

Result<std::string> ObjectStore::put_stash(std::string_view content, const std::string& rel_path,
                                           StashKind kind, std::int64_t time,
                                           std::int64_t index_gen, const std::string& batch_id)
{
    auto stored = put(content);
    if (stored.is_err())
    {
        return stored;
    }
    const std::string& hash = stored.value();

    // 自己記述サイドカー（D1：index.json 破損時に objects 走査だけで復元待ち一覧を再構築する）。
    // 内容アドレスで object を共有するため、サイドカーは退避単位の一意名にして後勝ち上書きを防ぐ
    // （内容一致の別退避＝別 relPath/kind でも両方の復元メタを残す）。
    const std::string disc = stash_discriminator(rel_path, kind, time, batch_id);
    json::Value meta = json::Value::object();
    meta.set("relPath", json::Value::str(rel_path));
    meta.set("kind", json::Value::str(to_string(kind)));
    meta.set("time", json::Value::integer(time));
    meta.set("indexGen", json::Value::integer(index_gen));
    meta.set("batchId", json::Value::str(batch_id));
    auto wrote = pika::util::write_atomic(stash_meta_path(hash, disc), meta.dump());
    if (wrote.is_err())
    {
        return Result<std::string>::err(wrote.error());
    }
    return Result<std::string>::ok(hash);
}

Result<std::string> ObjectStore::get(const std::string& hash) const
{
    // 不正な hash は object 不在として扱う（パス合成前の許可リスト検証。多層防御）。
    if (!is_valid_hash(hash))
    {
        return Result<std::string>::err(ErrorCode::NotFound, "object hash が不正です");
    }
    auto bytes = pika::util::read_all(object_path(hash));
    if (bytes.is_err())
    {
        return bytes; // NotFound / Io をそのまま伝播
    }
    return decompress(bytes.value());
}

bool ObjectStore::contains(const std::string& hash) const
{
    if (!is_valid_hash(hash))
    {
        return false;
    }
    std::error_code ec;
    return fs::exists(object_path(hash), ec) && !ec;
}

void ObjectStore::remove(const std::string& hash)
{
    if (!is_valid_hash(hash))
    {
        return; // 不正 hash はパス合成・削除に通さない（objects フォルダ外の誤削除を遮断）
    }
    std::error_code ec;
    fs::remove(object_path(hash), ec);
    // この object に紐づく全サイドカー（"<hash>.<disc>.meta"）を削除する。1 object に複数退避の
    // サイドカーがぶら下がり得るため、接頭辞 "<hash>." かつ接尾辞 ".meta"
    // のファイルを走査して消す。
    const std::string prefix = hash + ".";
    const std::string suffix = kMetaSuffix;
    std::error_code dec;
    if (!fs::exists(pika::util::utf8_to_path(dir_), dec) || dec)
    {
        return;
    }
    for (const auto& e : fs::directory_iterator(pika::util::utf8_to_path(dir_), dec))
    {
        if (dec)
        {
            break;
        }
        const std::string name = e.path().filename().string();
        const bool pfx =
            name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
        const bool sfx = name.size() >= suffix.size() &&
                         name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
        if (pfx && sfx)
        {
            std::error_code rec;
            fs::remove(e.path(), rec);
        }
    }
}

std::vector<std::string> ObjectStore::list_objects() const
{
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(pika::util::utf8_to_path(dir_), ec))
    {
        return out;
    }
    for (const auto& e : fs::directory_iterator(pika::util::utf8_to_path(dir_), ec))
    {
        if (ec)
        {
            break;
        }
        if (!e.is_regular_file())
        {
            continue;
        }
        const std::string name = e.path().filename().string();
        // サイドカー（*.meta）は object ではないため除外する。
        if (name.size() > std::string(kMetaSuffix).size() &&
            name.compare(name.size() - std::string(kMetaSuffix).size(),
                         std::string(kMetaSuffix).size(), kMetaSuffix) == 0)
        {
            continue;
        }
        out.push_back(name);
    }
    return out;
}

std::vector<RecoveredStash> ObjectStore::scan_recoverable_stashes() const
{
    std::vector<RecoveredStash> out;
    std::error_code ec;
    if (!fs::exists(pika::util::utf8_to_path(dir_), ec))
    {
        return out;
    }
    const std::string suffix = kMetaSuffix;
    for (const auto& e : fs::directory_iterator(pika::util::utf8_to_path(dir_), ec))
    {
        if (ec)
        {
            break;
        }
        if (!e.is_regular_file())
        {
            continue;
        }
        const std::string name = e.path().filename().string();
        if (name.size() <= suffix.size() ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
        {
            continue;
        }
        // ファイル名は "<objhash>.<disc>.meta"。先頭の '.' までを object hash として切り出す
        // （旧形式 "<objhash>.meta" もこの規則で objhash を取り出せる）。
        const std::size_t dot = name.find('.');
        if (dot == std::string::npos)
        {
            continue;
        }
        const std::string object_hash = name.substr(0, dot);
        // 対応 object が現存しないメタは復元不能のため除外する（退避＝最後の砦の到達性を担保）。
        if (!contains(object_hash))
        {
            continue;
        }
        auto meta_bytes = pika::util::read_all(pika::util::path_to_utf8(e.path()));
        if (meta_bytes.is_err())
        {
            continue;
        }
        json::Value meta;
        if (!json::parse(meta_bytes.value(), meta) || !meta.is_object())
        {
            continue; // 壊れたサイドカーは飛ばす（他の退避は保全する）
        }
        RecoveredStash rs;
        rs.object_hash = object_hash;
        if (const auto* p = meta.get("relPath"))
        {
            rs.rel_path = p->as_string();
        }
        StashKind kind = StashKind::Conflict;
        if (const auto* p = meta.get("kind"))
        {
            parse_stash_kind(p->as_string(), kind);
        }
        rs.kind = kind;
        if (const auto* p = meta.get("time"))
        {
            rs.time = p->as_int();
        }
        if (const auto* p = meta.get("indexGen"))
        {
            rs.index_gen = p->as_int();
        }
        if (const auto* p = meta.get("batchId"))
        {
            rs.batch_id = p->as_string();
        }
        out.push_back(std::move(rs));
    }
    // directory_iterator の順は不定なため、復元待ち一覧を決定的順序（新しい退避が先）に整える。
    // 同時刻は index 世代の新しい順、さらに object_hash で安定化する（上位 UI の提示順を一意化）。
    std::sort(out.begin(), out.end(), [](const RecoveredStash& a, const RecoveredStash& b) {
        if (a.time != b.time)
        {
            return a.time > b.time;
        }
        if (a.index_gen != b.index_gen)
        {
            return a.index_gen > b.index_gen;
        }
        return a.object_hash < b.object_hash;
    });
    return out;
}

} // namespace pika::core::snapshot
