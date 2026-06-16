#include "controller/document_controller.h"

#include "util/hash.h"

#include <algorithm>

namespace pika::controller
{

namespace doc = pika::core::document;
namespace snap = pika::core::snapshot;
namespace ws = pika::core::workspace;

using pika::util::ErrorCode;
using pika::util::Result;

Result<ConfirmOutcome> DocumentController::confirm(snap::SnapshotIndex& index,
                                                   ws::UnreadSet& unread,
                                                   const doc::ReviewTarget& target)
{
    // ディスク内容でベースラインを更新し未読を解除する（design.md 5.4 /
    // 要件8.3）。ReviewFlow::confirm
    // は内容を持てないファイル（10MB以上・画像・機密）でもハッシュベースラインを更新できる（D3）が、
    // 退避 object の put 失敗等で更新自体が失敗しうる。その Result
    // を握り潰さず、失敗時はベースラインを
    // 更新せず未読を維持してエラーを返す（データを失わない最上位原則の UI 側ガード）。
    auto updated = flow_.confirm(index, target);
    if (updated.is_err())
    {
        return Result<ConfirmOutcome>::err(updated.error());
    }

    // 更新に成功して初めて未読集合から除去する（ツリー/タブのマーク解除の素材）。
    unread.clear(target.rel_path);

    ConfirmOutcome out;
    out.rel_path = target.rel_path;
    out.unread_cleared = true;
    out.baseline_updated = true;
    return Result<ConfirmOutcome>::ok(out);
}

ConfirmAllOutcome DocumentController::confirm_all(snap::SnapshotIndex& index, ws::UnreadSet& unread,
                                                  const std::vector<doc::ReviewTarget>& targets,
                                                  const std::string& batch_id, std::int64_t time)
{
    // 開始時点の未読集合（targets）をフリーズして一括処理する（E3）。ReviewFlow::confirm_all は旧
    // ベースラインの baseline-replace 退避が失敗したファイルや、フリーズ後に並行書き込みで変わった
    // ファイルを skipped として返す（ベースラインを置き換えない＝未読のまま）。その分類をそのまま
    // 引き継ぎ、confirmed のみ未読集合から外す（skipped は未読を維持＝退避失敗を握り潰さない）。
    const doc::AllConfirmedResult res = flow_.confirm_all(index, targets, batch_id, time);

    ConfirmAllOutcome out;
    out.batch_id = res.batch_id;
    out.confirmed = res.confirmed;
    out.skipped = res.skipped;

    for (const std::string& rel : res.confirmed)
    {
        unread.clear(rel);
    }
    return out;
}

std::size_t DocumentController::revert_all(snap::SnapshotIndex& index, ws::UnreadSet& unread,
                                           const std::string& batch_id,
                                           const std::vector<std::string>& confirmed_rel_paths)
{
    // baseline-replace バッチを取り消すとベースラインが旧内容へ戻る＝確認前の状態に戻る。よって取消
    // 対象だったファイルは再び未読にする（ユーザーが「すべて確認済み」を取り消した＝未確認へ戻す）。
    const std::size_t reverted = flow_.revert_all(index, batch_id);
    if (reverted > 0)
    {
        for (const std::string& rel : confirmed_rel_paths)
        {
            unread.mark(rel);
        }
    }
    return reverted;
}

bool DocumentController::can_rollback(const snap::SnapshotIndex& index, const std::string& rel_path,
                                      const doc::FileContentClass& cls) const
{
    return flow_.can_rollback(index, rel_path, cls);
}

Result<std::string> DocumentController::rollback(snap::SnapshotIndex& index,
                                                 const doc::ReviewTarget& target)
{
    // 現ディスク内容を rollback
    // 退避してベースライン内容を返す（退避が先＝失われる内容を必ず残す）。 退避を取れない対象（内容
    // object を持たない）は ReviewFlow が Unsupported を返す。その Result を
    // 握り潰さずそのまま返す（呼び出し側が通知バー/ログへ変換する）。
    return flow_.rollback(index, target);
}

SavePlan DocumentController::prepare_save(snap::SnapshotIndex& index, const SaveContext& ctx)
{
    SavePlan plan;

    // 1) 表現可能性チェック（design.md 5.3 手順2 / G2）。現エンコーディングで表現できない文字を
    //    含むなら保存を中断する（別バイトへ静かに化けさせない。UTF-8/UTF-16 は常に true）。
    if (!pika::util::can_encode(ctx.buffer_content, ctx.encoding))
    {
        plan.decision = SaveDecision::BlockedEncoding;
        plan.error = {ErrorCode::Encoding,
                      "現在のエンコーディングで表現できない文字を含むため保存を中断しました"};
        return plan;
    }

    // 2) 衝突検知（design.md 5.3 手順1 /
    // F5）。キャッシュ値を使わず現ディスク内容のハッシュを再計算し、
    //    最後に読み込んだ時点のハッシュと異なれば衝突（外部に書き換えられた incoming がある）。
    const std::string disk_hash = pika::util::xxh3_64_lf_hex(ctx.disk_content);
    plan.conflict = !ctx.last_loaded_hash.empty() && disk_hash != ctx.last_loaded_hash;

    if (!plan.conflict)
    {
        // 衝突なし＝そのままアトミック置換に進んでよい。
        plan.decision = SaveDecision::Proceed;
        return plan;
    }

    // 3) 衝突あり。上書きすると外部変更（incoming）を失うため、退避してから上書きする（要件7.3）。
    //    退避を取れない対象（10MB以上・画像・機密）は既定でブロックする（退避不能ガード。手順3）。
    auto stashed =
        flow_.stash_incoming_before_save(index, ctx.rel_path, ctx.disk_content, ctx.cls, ctx.time);
    if (stashed.is_err())
    {
        // 退避不能ガード（Unsupported）と退避 I/O 失敗を弁別する。いずれもベースライン・ディスクを
        // 変更しない（データを失わない）。退避結合の Result を握り潰さず理由として返す。
        plan.decision = stashed.code() == ErrorCode::Unsupported ? SaveDecision::BlockedUnstashable
                                                                 : SaveDecision::BlockedStashFailed;
        plan.error = stashed.error();
        return plan;
    }

    // 退避できた＝外部変更を保全したうえで上書きしてよい。退避 object
    // 名を伝える（通知バーで「退避済み」 を示し、ユーザーが後から incoming
    // を復元できるようにする）。
    plan.decision = SaveDecision::Proceed;
    plan.stash_hash = stashed.value().hash;
    return plan;
}

} // namespace pika::controller
