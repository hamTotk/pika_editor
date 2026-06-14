#include "core/document/review_flow.h"

#include "util/hash.h"

#include <utility>

namespace pika::core::document
{

using pika::core::snapshot::IndexEntry;
using pika::core::snapshot::SnapshotIndex;
using pika::core::snapshot::StashEntry;
using pika::core::snapshot::StashKind;
using pika::core::snapshot::StashRequest;
using pika::util::ErrorCode;
using pika::util::Result;

namespace
{

// 退避不能ガードの共通実装（D2/D3 / 要件7.3）。内容 object を持てない対象（10MB以上・画像・機密）は
// 退避を取れないため、退避を前提にした破壊的操作（上書き保存・巻き戻し）を既定でブロックする。
Result<StashEntry> guard_stashable(const FileContentClass& cls)
{
    if (!cls.can_stash())
    {
        return Result<StashEntry>::err(
            ErrorCode::Unsupported,
            "退避を取れないファイル（10MB以上・画像・機密）への破壊的操作はブロックされます");
    }
    return Result<StashEntry>::ok(StashEntry{});
}

StashRequest make_request(const std::string& rel_path, StashKind kind, std::string_view content,
                          const std::string& batch_id)
{
    StashRequest req;
    req.rel_path = rel_path;
    req.kind = kind;
    req.content.assign(content.begin(), content.end());
    req.batch_id = batch_id;
    return req;
}

} // namespace

Result<StashEntry> ReviewFlow::stash_incoming_before_save(SnapshotIndex& index,
                                                          const std::string& rel_path,
                                                          std::string_view disk_content,
                                                          const FileContentClass& cls,
                                                          std::int64_t time)
{
    // 保存前に「現ディスク内容（外部に書き換えられた
    // incoming）」を退避してから上書きする（要件7.3）。
    if (auto guard = guard_stashable(cls); guard.is_err())
    {
        return guard;
    }
    return store_.add_stash(index, make_request(rel_path, StashKind::Incoming, disk_content, ""),
                            time);
}

Result<StashEntry> ReviewFlow::stash_conflict_before_take(SnapshotIndex& index,
                                                          const std::string& rel_path,
                                                          std::string_view my_edit,
                                                          const FileContentClass& cls,
                                                          std::int64_t time)
{
    // 外部変更を取り込む前に「自分の未保存編集（conflict）」を退避する（要件7.3）。
    if (auto guard = guard_stashable(cls); guard.is_err())
    {
        return guard;
    }
    return store_.add_stash(index, make_request(rel_path, StashKind::Conflict, my_edit, ""), time);
}

Result<IndexEntry> ReviewFlow::confirm(SnapshotIndex& index, const ReviewTarget& target)
{
    // ディスク内容でベースラインを更新し未読を解除する（要件8.3 / 5.4）。内容を持たない（10MB以上・
    // 画像・機密）ファイルはハッシュベースラインのみ更新する（D3。SnapshotStore に委譲）。
    return store_.set_baseline(index, target.rel_path, target.content, target.mtime,
                               target.cls.sensitive, target.cls.content_object_allowed);
}

bool ReviewFlow::can_rollback(const SnapshotIndex& index, const std::string& rel_path,
                              const FileContentClass& cls) const
{
    // 巻き戻しは「ベースライン内容 object を持つ」場合のみ提供する（要件8.3 / D3）。
    // can_stash=false（10MB以上・画像・機密）と、ベースライン未取得・ハッシュのみ記録は非活性。
    if (!cls.can_stash())
    {
        return false;
    }
    const IndexEntry* entry = index.find(rel_path);
    return entry != nullptr && !entry->baseline_object.empty();
}

Result<std::string> ReviewFlow::rollback(SnapshotIndex& index, const ReviewTarget& target)
{
    // 退避を取れない・ベースライン内容を持たない対象は巻き戻し非活性（要件8.3 / D3）。
    if (!can_rollback(index, target.rel_path, target.cls))
    {
        return Result<std::string>::err(
            ErrorCode::Unsupported,
            "退避を取れない（内容を保存しない）ファイルでは巻き戻しを提供しません");
    }

    // 巻き戻しで失われる「現在のディスク内容」を rollback 退避に保存してから（要件8.3。退避が先）、
    // ベースライン内容（巻き戻し先）を返す。ディスクへの書き戻しは呼び出し側が続けて行う。
    auto stashed = store_.add_stash(
        index, make_request(target.rel_path, StashKind::Rollback, target.content, ""),
        target.mtime);
    if (stashed.is_err())
    {
        return Result<std::string>::err(stashed.error());
    }
    return store_.restore_baseline(index, target.rel_path);
}

AllConfirmedResult ReviewFlow::confirm_all(SnapshotIndex& index,
                                           const std::vector<ReviewTarget>& targets,
                                           const std::string& batch_id, std::int64_t time)
{
    AllConfirmedResult result;
    result.batch_id = batch_id;

    // 開始時点の未読集合（targets）をフリーズして処理する（要件8.3 / E3）。各対象について、フリーズ
    // 時点の内容ハッシュ（freeze_hash）と現内容のハッシュが不一致なら、処理中に並行書き込みで内容が
    // 変わった＝ユーザーが見ていない未確認内容なのでスキップして未読のまま残す。さらに既にベースラインと
    // 一致（＝確認済みで実質未読でない）ファイルも更新不要としてスキップする。
    for (const auto& target : targets)
    {
        const std::string current_hash = pika::util::xxh3_64_lf_hex(target.content);

        // フリーズ時点と現在で内容が変わったファイルはスキップ（未確認内容をベースライン化しない）。
        if (!target.freeze_hash.empty() && target.freeze_hash != current_hash)
        {
            result.skipped.push_back(target.rel_path);
            continue;
        }

        const IndexEntry* before = index.find(target.rel_path);
        const std::string baseline_at_start =
            before != nullptr ? before->baseline_hash : std::string{};
        const std::string old_baseline_object =
            before != nullptr ? before->baseline_object : std::string{};

        // 既にベースラインと一致（＝確認済みで未読でない）ファイルは更新不要としてスキップする。
        if (baseline_at_start == current_hash)
        {
            result.skipped.push_back(target.rel_path);
            continue;
        }

        // 旧ベースライン内容（object を持つ場合のみ）を baseline-replace 退避（同一
        // batch_id）に保存 してから（一括取消の起点。10件 LRU
        // 枠とは別保持）ベースラインを更新する。内容 object を持た
        // ない（ハッシュのみ）旧ベースラインは退避できないが、ベースライン更新自体は可能（D3）。
        if (target.cls.can_stash() && !old_baseline_object.empty())
        {
            auto old_content = store_.restore_baseline(index, target.rel_path);
            if (old_content.is_err())
            {
                // 旧ベースライン内容を取り出せない（object 欠落・I/O
                // 障害等）＝一括取消（revert_all）の
                // 起点を作れない。退避を取らずにベースラインを置き換えると旧内容へ戻せなくなるため、
                // 更新せずスキップして未読のまま残す（設計原則1「データを失わない」／要件8.3 の一括
                // 取消保証を死守する）。
                result.skipped.push_back(target.rel_path);
                continue;
            }
            auto stashed =
                store_.add_stash(index,
                                 make_request(target.rel_path, StashKind::BaselineReplace,
                                              old_content.value(), batch_id),
                                 time);
            if (stashed.is_err())
            {
                // 旧ベースラインを baseline-replace 退避に保存できなかった。退避失敗を黙殺して
                // ベースラインを置き換えると、この確認を一括取消で戻せなくなる（取消保証が破れる）。
                // 同じく更新を行わずスキップし未読のまま残す。
                result.skipped.push_back(target.rel_path);
                continue;
            }
        }

        auto updated = store_.set_baseline(index, target.rel_path, target.content, target.mtime,
                                           target.cls.sensitive, target.cls.content_object_allowed);
        if (updated.is_ok())
        {
            result.confirmed.push_back(target.rel_path);
        }
        else
        {
            // 更新に失敗したら未読のまま残す（データを失わない側へ倒す）。
            result.skipped.push_back(target.rel_path);
        }
    }
    return result;
}

std::size_t ReviewFlow::revert_all(SnapshotIndex& index, const std::string& batch_id)
{
    // baseline-replace バッチの一括取消（旧ベースラインの復元起点。SnapshotStore
    // に委譲。要件8.3）。
    return store_.revert_batch(index, batch_id);
}

} // namespace pika::core::document
