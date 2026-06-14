#include "core/snapshot/snapshot_store.h"

#include "core/snapshot/index_io.h"
#include "core/snapshot/secure_dir.h"
#include "util/hash.h"
#include "util/path_util.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>

namespace pika::core::snapshot
{

namespace fs = std::filesystem;
using pika::util::ErrorCode;
using pika::util::Result;

std::string workspace_key(std::string_view normalized_path)
{
    return pika::util::xxh3_64_hex(normalized_path);
}

std::string file_key(std::string_view normalized_file_path)
{
    return "file-" + pika::util::xxh3_64_hex(normalized_file_path);
}

namespace
{

std::string join(const std::string& a, const std::string& b)
{
    return pika::util::path_to_utf8(pika::util::utf8_to_path(a) / pika::util::utf8_to_path(b));
}

} // namespace

void SnapshotStore::apply_per_file_lru(IndexEntry& entry, std::int64_t now) const
{
    // 未復元かつ生成から protect_seconds（14日）以内の退避は保護対象＝LRU でも削除しない
    // （要件9.3。「データを失わない／退避＝最後の砦」が LRU の10件枠より優先）。
    auto is_protected = [&](const StashEntry& s) {
        return !s.restored && (now - s.time) <= policy_.protect_seconds;
    };

    // baseline-replace は別バッチ枠のため10件枠の対象外。非バッチ退避だけを数える。
    std::size_t non_batch = 0;
    for (const auto& s : entry.stash)
    {
        if (s.kind != StashKind::BaselineReplace)
        {
            ++non_batch;
        }
    }
    if (non_batch <= policy_.per_file_stash_limit)
    {
        return;
    }

    std::size_t to_drop = non_batch - policy_.per_file_stash_limit;
    // 古い（先頭）側の非バッチかつ非保護の退避から落とす。保護退避は枠超過でも残すため、
    // 未復元・14日以内の退避が11件以上積まれた場合は10件を超えても削除されない。
    for (auto it = entry.stash.begin(); it != entry.stash.end() && to_drop > 0;)
    {
        if (it->kind != StashKind::BaselineReplace && !is_protected(*it))
        {
            it = entry.stash.erase(it);
            --to_drop;
        }
        else
        {
            ++it;
        }
    }
}

SnapshotStore::SnapshotStore(std::string snapshots_root, std::string ws_key)
    : SnapshotStore(std::move(snapshots_root), std::move(ws_key), CapacityPolicy{})
{
}

SnapshotStore::SnapshotStore(std::string snapshots_root, std::string ws_key, CapacityPolicy policy)
    : ws_dir_(join(snapshots_root, ws_key)), objects_(join(ws_dir_, "objects")), policy_(policy)
{
}

std::string SnapshotStore::index_path() const
{
    return join(ws_dir_, "index.json");
}

Result<SnapshotIndex> SnapshotStore::load()
{
    return load_index(index_path());
}

Result<void> SnapshotStore::save(const SnapshotIndex& index)
{
    // snapshots フォルダ・index・objects は本人のみアクセス可能な ACL で作る（要件9.1）。
    create_secure_dir(ws_dir_);
    create_secure_dir(objects_.dir());
    return save_index(index_path(), index);
}

Result<IndexEntry> SnapshotStore::set_baseline(SnapshotIndex& index, const std::string& rel_path,
                                               std::string_view content, std::int64_t mtime,
                                               bool sensitive, bool content_object_allowed)
{
    // 失敗時に entry を半端変異させない（原子性）。新しいベースライン値はローカルに組み立て、
    // fallible な内容 object 確保（put）が確定してから一括コミットする。put が I/O 失敗しても
    // entry を一切触らずに err を返すため、呼び出し側（ReviewFlow::confirm/confirm_all）の
    // 「失敗時は旧状態維持・未読維持」契約が真に成立する（設計原則1「データを失わない」）。
    const std::string new_hash = pika::util::xxh3_64_lf_hex(content);

    // 機密ファイル・内容保存不可（10MB以上/画像）は内容 object を持たず baselineHash のみ記録する
    // （要件9.1/9.2。元ファイル削除後に平文コピーを残さないデータ最小化）。
    std::string new_object; // ハッシュのみ記録なら空のまま
    if (!(sensitive || !content_object_allowed))
    {
        create_secure_dir(objects_.dir());
        auto stored = objects_.put(content);
        if (stored.is_err())
        {
            return Result<IndexEntry>::err(stored.error()); // entry 未変異で返す
        }
        new_object = stored.value();
    }

    // ここから先は失敗しない。entry を確定値で一括コミットする（無ければ作る）。
    IndexEntry* entry = index.find(rel_path);
    if (entry == nullptr)
    {
        index.entries.push_back(IndexEntry{});
        entry = &index.entries.back();
        entry->rel_path = rel_path;
    }
    entry->baseline_hash = new_hash;
    entry->baseline_mtime = mtime;
    entry->baseline_size = content.size();
    entry->baseline_object = new_object; // ハッシュのみ記録なら空＝旧 object を確実に外す
    entry->unread = false;               // 確認済み = ベースライン更新で未読解除
    return Result<IndexEntry>::ok(*entry);
}

Result<std::string> SnapshotStore::restore_baseline(const SnapshotIndex& index,
                                                    const std::string& rel_path) const
{
    const IndexEntry* entry = index.find(rel_path);
    if (entry == nullptr)
    {
        return Result<std::string>::err(ErrorCode::NotFound, "ベースラインがありません");
    }
    if (entry->baseline_object.empty())
    {
        // ハッシュのみ記録（機密・10MB以上・画像）は内容復元・差分・巻き戻し不可（D2）。
        return Result<std::string>::err(ErrorCode::Unsupported,
                                        "内容を持たないベースラインは復元できません");
    }
    return objects_.get(entry->baseline_object);
}

Result<StashEntry> SnapshotStore::add_stash(SnapshotIndex& index, const StashRequest& req,
                                            std::int64_t time)
{
    IndexEntry* entry = index.find(req.rel_path);
    if (entry == nullptr)
    {
        index.entries.push_back(IndexEntry{});
        entry = &index.entries.back();
        entry->rel_path = req.rel_path;
    }

    create_secure_dir(ws_dir_);
    create_secure_dir(objects_.dir());

    const bool is_batch = req.kind == StashKind::BaselineReplace;
    const std::string batch_id = is_batch ? req.batch_id : std::string{};

    auto stored = objects_.put_stash(req.content, req.rel_path, req.kind, time,
                                     static_cast<std::int64_t>(index.version), batch_id);
    if (stored.is_err())
    {
        return Result<StashEntry>::err(stored.error());
    }

    StashEntry s;
    s.hash = stored.value();
    s.time = time;
    s.kind = req.kind;
    s.batch_id = batch_id;
    s.restored = false;
    entry->stash.push_back(s);

    // ファイルごと最新10件 LRU（要件9.2）。enforce_capacity と同じ規則（保護退避を侵さない）を
    // 共有し、二経路の乖離を防ぐ。退避の生成時刻 time を保護判定の基準時とする。あふれた退避
    // object は他からの参照が無くなれば mark-and-sweep が回収する。
    const std::size_t before = entry->stash.size();
    apply_per_file_lru(*entry, time);
    if (entry->stash.size() != before)
    {
        sweep_unreferenced_objects(index);
    }

    return Result<StashEntry>::ok(s);
}

Result<std::string> SnapshotStore::restore_stash(const std::string& object_hash) const
{
    return objects_.get(object_hash);
}

std::size_t SnapshotStore::revert_batch(SnapshotIndex& index, const std::string& batch_id)
{
    if (batch_id.empty())
    {
        return 0;
    }
    // 「すべて確認済み」の一括取消（要件8.3）。baseline-replace 退避は単に消すのではなく、退避が
    // 保持する旧ベースライン内容で各ファイルのベースラインを復元してから退避を外す。単に erase する
    // だけだと取消が no-op になり、さらに sweep で旧 object が物理削除されて復元点が永久に失われる
    // （設計原則1「データを失わない／退避＝最後の砦」違反）。confirm_all は対象ごとに高々 1 件の
    // baseline-replace を積むため、各エントリの該当退避を順に旧ベースラインとして適用する。
    std::size_t reverted = 0;
    for (auto& entry : index.entries)
    {
        for (auto it = entry.stash.begin(); it != entry.stash.end();)
        {
            if (it->kind == StashKind::BaselineReplace && it->batch_id == batch_id)
            {
                auto old_content = objects_.get(it->hash);
                if (old_content.is_ok())
                {
                    // 旧ベースラインへ復元する。退避 object は既存なので hash を付け替えるだけで
                    // 再書き込みしない。ハッシュ・サイズは内容から再計算。mtime は退避が保持しない
                    // ため据え置く（差分基準はハッシュ照合が主。watcher 側で再確定される）。確認の
                    // 取消なので未読に戻す（復元したベースラインと現ディスク内容は再び相違し得る）。
                    entry.baseline_object = it->hash;
                    entry.baseline_hash = pika::util::xxh3_64_lf_hex(old_content.value());
                    entry.baseline_size = old_content.value().size();
                    entry.unread = true;
                    it = entry.stash.erase(it);
                    ++reverted;
                    continue;
                }
                // 旧内容を取り出せない場合は退避を残しベースラインを変えない（データを失わない側）。
            }
            ++it;
        }
    }
    if (reverted > 0)
    {
        // 復元で参照が外れた「新ベースライン object」など未参照 object を回収する（復元した旧
        // object は baseline_object が指すため sweep 対象外）。
        sweep_unreferenced_objects(index);
    }
    return reverted;
}

std::size_t SnapshotStore::enforce_capacity(SnapshotIndex& index, std::int64_t now,
                                            std::int64_t last_opened)
{
    // 適用順は「ファイルごと最新10件のLRU → 全体容量GC」。90日GC は last_opened 基準（要件9.3）。
    // 未復元かつ生成から protect_seconds（14日）以内の退避は保護対象から外さない。
    auto is_protected = [&](const StashEntry& s) {
        return !s.restored && (now - s.time) <= policy_.protect_seconds;
    };

    // 1) ファイルごと最新10件 LRU（baseline-replace は別枠で対象外）。保護退避は LRU でも残す
    //    （add_stash と同一規則。apply_per_file_lru に集約）。
    for (auto& entry : index.entries)
    {
        apply_per_file_lru(entry, now);
    }

    // 2) 90日GC：last_opened から age_gc_seconds 経過時、保護対象外の退避を削除する
    //    （ベースラインは削除対象外）。
    if (now - last_opened > policy_.age_gc_seconds)
    {
        for (auto& entry : index.entries)
        {
            entry.stash.erase(std::remove_if(entry.stash.begin(), entry.stash.end(),
                                             [&](const StashEntry& s) { return !is_protected(s); }),
                              entry.stash.end());
        }
    }

    // 3) 全体容量GC：object 実体の合計が total_byte_limit を超える間、保護対象外の古い退避から
    //    削除する。保護分だけで上限を超える場合は削除せず残す（通知バーで促す＝UI 側責務）。
    //
    //    object サイズは削除のたびに再 syscall せず、ループ前に 1 度だけ集計して保持する（最悪
    //    O(V·R) syscall を避け「固まらない」を守る）。共有 object は参照カウントで管理し、最後の
    //    参照退避が消えたときだけ合計から減算する（重複排除を尊重）。
    std::map<std::string, std::uint64_t> object_size; // hash -> bytes（実在 object のみ）
    std::map<std::string, std::size_t> ref_count;     // hash -> index 上の参照数
    {
        std::error_code ec;
        for (const auto& entry : index.entries)
        {
            if (!entry.baseline_object.empty())
            {
                ++ref_count[entry.baseline_object];
            }
            for (const auto& s : entry.stash)
            {
                ++ref_count[s.hash];
            }
        }
        for (const auto& [h, count] : ref_count)
        {
            (void)count;
            const auto p = pika::util::utf8_to_path(objects_.dir()) / h;
            const auto sz = fs::file_size(p, ec);
            object_size[h] = ec ? 0 : sz;
            ec.clear();
        }
    }
    auto current_total = [&]() -> std::uint64_t {
        std::uint64_t total = 0;
        for (const auto& [h, count] : ref_count)
        {
            if (count > 0)
            {
                auto it = object_size.find(h);
                if (it != object_size.end())
                {
                    total += it->second;
                }
            }
        }
        return total;
    };

    std::uint64_t total = current_total();
    while (total > policy_.total_byte_limit)
    {
        // 全エントリの非保護退避から最古を 1
        // 件選んで削除する。無ければ打ち切り（保護分が上限超過）。
        IndexEntry* victim_entry = nullptr;
        std::size_t victim_idx = 0;
        std::int64_t oldest = 0;
        bool found = false;
        for (auto& entry : index.entries)
        {
            for (std::size_t i = 0; i < entry.stash.size(); ++i)
            {
                if (is_protected(entry.stash[i]))
                {
                    continue;
                }
                if (!found || entry.stash[i].time < oldest)
                {
                    found = true;
                    oldest = entry.stash[i].time;
                    victim_entry = &entry;
                    victim_idx = i;
                }
            }
        }
        if (!found)
        {
            break; // 保護退避のみが残存：上限超過でも削除せず通知に委ねる
        }
        const std::string victim_hash = victim_entry->stash[victim_idx].hash;
        victim_entry->stash.erase(victim_entry->stash.begin() +
                                  static_cast<std::ptrdiff_t>(victim_idx));
        // 参照カウントを 1 減らし、0 になったら（最後の参照が消えたら）合計から減算する。
        auto rc = ref_count.find(victim_hash);
        if (rc != ref_count.end() && rc->second > 0)
        {
            --rc->second;
            if (rc->second == 0)
            {
                const auto sz = object_size.find(victim_hash);
                if (sz != object_size.end())
                {
                    total -= (sz->second <= total ? sz->second : total);
                }
            }
        }
    }

    return sweep_unreferenced_objects(index);
}

std::size_t SnapshotStore::sweep_unreferenced_objects(const SnapshotIndex& index)
{
    // mark：index 上の全ベースライン object と全退避 object を参照集合に集める。
    std::set<std::string> referenced;
    for (const auto& entry : index.entries)
    {
        if (!entry.baseline_object.empty())
        {
            referenced.insert(entry.baseline_object);
        }
        for (const auto& s : entry.stash)
        {
            if (!s.hash.empty())
            {
                referenced.insert(s.hash);
            }
        }
    }

    // sweep：実在 object のうち参照集合に無いものを物理削除する（共有実体の誤削除防止。D5）。
    std::size_t removed = 0;
    for (const auto& obj : objects_.list_objects())
    {
        if (referenced.find(obj) == referenced.end())
        {
            objects_.remove(obj);
            ++removed;
        }
    }
    return removed;
}

void SnapshotStore::purge()
{
    std::error_code ec;
    fs::remove_all(pika::util::utf8_to_path(ws_dir_),
                   ec); // index・objects ごと一括削除（手動パージ。要件9.4）
}

std::vector<RecoveredStash> SnapshotStore::recover_pending_stashes() const
{
    return objects_.scan_recoverable_stashes();
}

} // namespace pika::core::snapshot
