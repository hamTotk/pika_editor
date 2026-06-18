// core/document ReviewFlow の検証（sprint10 must の中核：退避フロー結合）。
//  - 退避結合：incoming（保存衝突）・conflict（取り込み）・rollback（巻き戻し前）・
//    baseline-replace（すべて確認済み）の各退避が発生し、復元で原内容と一致する（D6）
//  - index破損後に objects 走査から退避が復元できる（D1。ReviewFlow が積んだ退避を SnapshotStore
//    の復元経路で観測する）
//  - 確認済み：ディスク内容でベースライン更新・未読解除。内容を持たない（10MB以上/画像）は
//    ハッシュベースラインのみ更新（D3）
//  - 巻き戻し：退避を取れないファイル（10MB以上/画像）では巻き戻しを提供しない（非活性。D3）
//  - すべて確認済み：開始時未読集合フリーズ・処理中変化はスキップ・batchId 一括取消（should）
//  - 退避不能ガード：退避を取れないファイルへの破壊的操作が既定でブロックされる（should）
// すべてテンポラリのデータルートを使った実 FS で観測する（design.md 13章 / 要件7.3・8.3・9章）。
#include "core/document/review_flow.h"

#include "core/snapshot/snapshot_store.h"
#include "core/snapshot/snapshot_types.h"
#include "util/atomic_file.h"
#include "util/hash.h"

#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace
{

namespace fs = std::filesystem;

using pika::core::document::AllConfirmedResult;
using pika::core::document::FileContentClass;
using pika::core::document::ReviewFlow;
using pika::core::document::ReviewTarget;
using pika::core::snapshot::SnapshotIndex;
using pika::core::snapshot::SnapshotStore;
using pika::core::snapshot::StashKind;
using pika::core::snapshot::workspace_key;
using pika::util::ErrorCode;

class ReviewFlowTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root_ = fs::temp_directory_path() /
                ("pika_reviewflow_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(root_);
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    std::string snapshots_root() const { return (root_ / "snapshots").string(); }
    fs::path root_;
};

FileContentClass stashable()
{
    return FileContentClass{};
}
FileContentClass large_or_image()
{
    FileContentClass c;
    c.content_object_allowed = false;
    return c;
}
ReviewTarget target(const std::string& rel, const std::string& content, std::int64_t mtime,
                    FileContentClass cls = stashable())
{
    ReviewTarget t;
    t.rel_path = rel;
    t.content = content;
    t.mtime = mtime;
    t.cls = cls;
    return t;
}

// --- 退避結合：4 種別の退避と復元 -------------------------------------------

TEST_F(ReviewFlowTest, IncomingStashSavesAndRestores)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;

    const std::string disk = "外部に書き換えられたディスク内容\n";
    auto stashed = flow.stash_incoming_before_save(index, "a.md", disk, stashable(), 100);
    ASSERT_TRUE(stashed.is_ok());
    EXPECT_EQ(stashed.value().kind, StashKind::Incoming);

    auto restored = store.restore_stash(stashed.value().hash);
    ASSERT_TRUE(restored.is_ok());
    EXPECT_EQ(restored.value(), disk); // 復元で原内容と一致（D6）
}

TEST_F(ReviewFlowTest, ConflictStashSavesAndRestores)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;

    const std::string my_edit = "取り込む前の自分の未保存編集\n";
    auto stashed = flow.stash_conflict_before_take(index, "a.md", my_edit, stashable(), 200);
    ASSERT_TRUE(stashed.is_ok());
    EXPECT_EQ(stashed.value().kind, StashKind::Conflict);

    auto restored = store.restore_stash(stashed.value().hash);
    ASSERT_TRUE(restored.is_ok());
    EXPECT_EQ(restored.value(), my_edit);
}

TEST_F(ReviewFlowTest, RollbackStashesCurrentAndReturnsBaseline)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;

    const std::string baseline = "確認済みベースライン内容\n";
    ASSERT_TRUE(store.set_baseline(index, "a.md", baseline, 1, false, true).is_ok());

    const std::string current = "巻き戻しで失われる現在内容\n";
    auto rolled = flow.rollback(index, target("a.md", current, 300));
    ASSERT_TRUE(rolled.is_ok());
    // 巻き戻し先＝ベースライン内容を返す（呼び出し側がディスクへ書き戻す）。
    EXPECT_EQ(rolled.value(), baseline);

    // 巻き戻しで失われる現在内容は rollback 退避として復元できる（要件8.3受け入れ基準）。
    const auto* e = index.find("a.md");
    ASSERT_NE(e, nullptr);
    bool found_rollback = false;
    for (const auto& s : e->stash)
    {
        if (s.kind == StashKind::Rollback)
        {
            auto body = store.restore_stash(s.hash);
            ASSERT_TRUE(body.is_ok());
            EXPECT_EQ(body.value(), current);
            found_rollback = true;
        }
    }
    EXPECT_TRUE(found_rollback);
}

TEST_F(ReviewFlowTest, BaselineReplaceStashSavesOldBaseline)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;

    const std::string old_baseline = "旧ベースライン\n";
    ASSERT_TRUE(store.set_baseline(index, "a.md", old_baseline, 1, false, true).is_ok());

    const std::string new_content = "新しく確認した内容\n";
    AllConfirmedResult r =
        flow.confirm_all(index, {target("a.md", new_content, 2)}, "batch-X", 500);
    ASSERT_EQ(r.confirmed.size(), 1u);

    // baseline-replace 退避に旧ベースラインが保存され、復元で原内容と一致する（D6）。
    const auto* e = index.find("a.md");
    ASSERT_NE(e, nullptr);
    bool found = false;
    for (const auto& s : e->stash)
    {
        if (s.kind == StashKind::BaselineReplace && s.batch_id == "batch-X")
        {
            auto body = store.restore_stash(s.hash);
            ASSERT_TRUE(body.is_ok());
            EXPECT_EQ(body.value(), old_baseline);
            found = true;
        }
    }
    EXPECT_TRUE(found);

    // 新ベースラインへ更新されている。
    auto cur = store.restore_baseline(index, "a.md");
    ASSERT_TRUE(cur.is_ok());
    EXPECT_EQ(cur.value(), new_content);
}

// --- index破損後に objects 走査から退避が復元できる（D1） ---------------------

TEST_F(ReviewFlowTest, StashesRecoverableAfterIndexCorruption)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;

    ASSERT_TRUE(
        flow.stash_incoming_before_save(index, "a.md", "incoming A", stashable(), 10).is_ok());
    ASSERT_TRUE(
        flow.stash_conflict_before_take(index, "b.md", "conflict B", stashable(), 20).is_ok());
    ASSERT_TRUE(store.save(index).is_ok());

    // index.json を破損させても、ReviewFlow が積んだ退避は objects 走査から復元できる（最後の砦）。
    ASSERT_TRUE(pika::util::write_atomic(store.index_path(), "{ corrupt }").is_ok());
    ASSERT_TRUE(store.load().is_err());

    auto pending = store.recover_pending_stashes();
    ASSERT_EQ(pending.size(), 2u);
    bool a_ok = false;
    bool b_ok = false;
    for (const auto& p : pending)
    {
        auto body = store.restore_stash(p.object_hash);
        ASSERT_TRUE(body.is_ok());
        if (p.rel_path == "a.md")
        {
            a_ok = (body.value() == "incoming A");
        }
        if (p.rel_path == "b.md")
        {
            b_ok = (body.value() == "conflict B");
        }
    }
    EXPECT_TRUE(a_ok);
    EXPECT_TRUE(b_ok);
}

// --- 確認済み：ディスク内容でベースライン更新・未読解除 ----------------------

TEST_F(ReviewFlowTest, ConfirmUpdatesBaselineFromDiskAndClearsUnread)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;
    // 未読のエントリを用意する。
    ASSERT_TRUE(store.set_baseline(index, "a.md", "古い内容\n", 1, false, true).is_ok());
    index.find("a.md")->unread = true;

    const std::string disk = "確認したディスク内容\n";
    auto e = flow.confirm(index, target("a.md", disk, 99));
    ASSERT_TRUE(e.is_ok());
    EXPECT_FALSE(e.value().unread);                                       // 未読解除
    EXPECT_EQ(e.value().baseline_hash, pika::util::xxh3_64_lf_hex(disk)); // ディスク内容で更新

    auto restored = store.restore_baseline(index, "a.md");
    ASSERT_TRUE(restored.is_ok());
    EXPECT_EQ(restored.value(), disk);
}

TEST_F(ReviewFlowTest, ConfirmLargeOrImageUpdatesHashBaselineOnly)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;

    const std::string disk = "巨大ファイル相当のマーカー";
    auto e = flow.confirm(index, target("big.bin", disk, 1, large_or_image()));
    ASSERT_TRUE(e.is_ok());
    EXPECT_FALSE(e.value().unread);
    EXPECT_FALSE(e.value().baseline_hash.empty());  // ハッシュベースラインは記録
    EXPECT_TRUE(e.value().baseline_object.empty()); // 内容 object は持たない（D3）
    EXPECT_TRUE(store.objects().list_objects().empty());

    // 内容を持たないため差分・巻き戻しは非活性（Unsupported）。
    auto restored = store.restore_baseline(index, "big.bin");
    ASSERT_TRUE(restored.is_err());
    EXPECT_EQ(restored.code(), ErrorCode::Unsupported);
}

// --- 確定直前の再照合（F-015 / design 5.4 E2） ------------------------------

TEST_F(ReviewFlowTest, ConfirmRediffMatchUpdatesBaseline)
{
    // expected_hash
    // が現ディスク内容（content）と一致＝ユーザーが見た内容のまま。ベースライン更新成功。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "a.md", "古い内容\n", 1, false, true).is_ok());
    index.find("a.md")->unread = true;

    const std::string disk = "確認したディスク内容\n";
    ReviewTarget t = target("a.md", disk, 99);
    t.expected_hash = pika::util::xxh3_64_lf_hex(disk); // 見た内容＝現ディスク内容

    auto e = flow.confirm(index, t);
    ASSERT_TRUE(e.is_ok());
    EXPECT_FALSE(e.value().unread);
    EXPECT_EQ(e.value().baseline_hash, pika::util::xxh3_64_lf_hex(disk)); // ディスク内容で更新
}

TEST_F(ReviewFlowTest, ConfirmRediffMismatchCancelsWithoutBaselineChange)
{
    // expected_hash が現ディスク内容と不一致＝差分描画後に外部変更が入った。Cancelled で中断し
    // ベースラインは旧のまま（未読維持できる前提。設計原則1「データを失わない」）。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "a.md", "旧ベースライン\n", 1, false, true).is_ok());
    const std::string old_hash = index.find("a.md")->baseline_hash;
    index.find("a.md")->unread = true;

    const std::string disk = "外部変更後の現ディスク内容\n";
    ReviewTarget t = target("a.md", disk, 99);
    t.expected_hash = pika::util::xxh3_64_lf_hex("ユーザーが見ていた古い内容\n"); // 不一致

    auto e = flow.confirm(index, t);
    ASSERT_TRUE(e.is_err());
    EXPECT_EQ(e.code(), ErrorCode::Cancelled);              // 中断（再差分は呼び出し側）
    EXPECT_EQ(index.find("a.md")->baseline_hash, old_hash); // ベースライン未変更
    EXPECT_TRUE(index.find("a.md")->unread);                // 未読は維持
}

TEST_F(ReviewFlowTest, ConfirmEmptyExpectedHashSkipsRediff)
{
    // expected_hash 空＝再照合しない（タブ未オープン等）。後方互換で従来どおり更新成功する。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "a.md", "古い内容\n", 1, false, true).is_ok());
    index.find("a.md")->unread = true;

    const std::string disk = "確認したディスク内容\n";
    ReviewTarget t = target("a.md", disk, 99); // expected_hash は空のまま

    auto e = flow.confirm(index, t);
    ASSERT_TRUE(e.is_ok());
    EXPECT_FALSE(e.value().unread);
    EXPECT_EQ(e.value().baseline_hash, pika::util::xxh3_64_lf_hex(disk));
}

// --- 巻き戻し非活性判定（D3） ------------------------------------------------

TEST_F(ReviewFlowTest, CanRollbackOnlyWhenBaselineObjectPresent)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;

    // 内容 object を持つテキスト → 巻き戻し可能。
    ASSERT_TRUE(store.set_baseline(index, "a.md", "本文\n", 1, false, true).is_ok());
    EXPECT_TRUE(flow.can_rollback(index, "a.md", stashable()));

    // 10MB以上・画像（content_object_allowed=false）→ 巻き戻し非活性。
    ASSERT_TRUE(store.set_baseline(index, "big.bin", "marker", 1, false, false).is_ok());
    EXPECT_FALSE(flow.can_rollback(index, "big.bin", large_or_image()));

    // ベースライン未取得 → 巻き戻し非活性。
    EXPECT_FALSE(flow.can_rollback(index, "unknown.md", stashable()));
}

TEST_F(ReviewFlowTest, RollbackBlockedForLargeOrImage)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "big.bin", "marker", 1, false, false).is_ok());

    auto rolled = flow.rollback(index, target("big.bin", "現内容", 2, large_or_image()));
    ASSERT_TRUE(rolled.is_err());
    EXPECT_EQ(rolled.code(), ErrorCode::Unsupported); // 巻き戻しを提供しない（非活性）
    // 破壊的退避も積まれていない。
    EXPECT_TRUE(store.objects().list_objects().empty());
}

// --- 退避不能ガード（should） ------------------------------------------------

TEST_F(ReviewFlowTest, IncomingStashBlockedForLargeOrImage)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;

    auto r = flow.stash_incoming_before_save(index, "big.bin", "disk", large_or_image(), 1);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.code(), ErrorCode::Unsupported);
    EXPECT_TRUE(store.objects().list_objects().empty());
}

TEST_F(ReviewFlowTest, ConflictStashBlockedForSensitive)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;

    FileContentClass secret;
    secret.sensitive = true;
    auto r = flow.stash_conflict_before_take(index, ".env", "SECRET=1", secret, 1);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.code(), ErrorCode::Unsupported);
    EXPECT_TRUE(store.objects().list_objects().empty()); // 平文が一切書かれない
}

// --- すべて確認済み：フリーズ・スキップ・一括取消（should） ------------------

TEST_F(ReviewFlowTest, ConfirmAllSkipsFilesChangedDuringFreeze)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;

    // a.md：未読（ベースラインと現内容が異なる）→ 確認対象。
    ASSERT_TRUE(store.set_baseline(index, "a.md", "古いA\n", 1, false, true).is_ok());
    // c.md：既にベースライン＝現内容（確認済み相当）→ スキップされる。
    const std::string c_content = "変わらないC\n";
    ASSERT_TRUE(store.set_baseline(index, "c.md", c_content, 1, false, true).is_ok());

    std::vector<ReviewTarget> targets = {
        target("a.md", "新しいA\n", 2),
        target("c.md", c_content, 2), // ベースラインと一致 → スキップ
    };
    AllConfirmedResult r = flow.confirm_all(index, targets, "batch-1", 10);

    EXPECT_EQ(r.batch_id, "batch-1");
    ASSERT_EQ(r.confirmed.size(), 1u);
    EXPECT_EQ(r.confirmed[0], "a.md");
    ASSERT_EQ(r.skipped.size(), 1u);
    EXPECT_EQ(r.skipped[0], "c.md");

    // a.md は新ベースラインへ。c.md は据え置き（スキップ＝据え置き）。
    auto a = store.restore_baseline(index, "a.md");
    ASSERT_TRUE(a.is_ok());
    EXPECT_EQ(a.value(), "新しいA\n");
}

TEST_F(ReviewFlowTest, ConfirmAllSkipsFileChangedSinceFreeze)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "a.md", "旧A\n", 1, false, true).is_ok());

    // フリーズ時点で見ていた内容（freeze_hash）と、確定直前に読んだ現内容（content）が食い違う＝
    // 処理中に並行書き込みで変わったケース。未確認内容をベースライン化しないようスキップする。
    ReviewTarget t = target("a.md", "確定直前に並行書き込みされた別内容\n", 2);
    t.freeze_hash = pika::util::xxh3_64_lf_hex("フリーズ時に見ていた内容\n");

    AllConfirmedResult r = flow.confirm_all(index, {t}, "batch-2", 10);
    EXPECT_TRUE(r.confirmed.empty());
    ASSERT_EQ(r.skipped.size(), 1u);
    EXPECT_EQ(r.skipped[0], "a.md");

    // ベースラインは旧Aのまま（未確認内容で汚染されない）。
    auto a = store.restore_baseline(index, "a.md");
    ASSERT_TRUE(a.is_ok());
    EXPECT_EQ(a.value(), "旧A\n");
}

TEST_F(ReviewFlowTest, ConfirmAllConfirmsWhenFreezeHashMatchesCurrent)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "a.md", "旧A\n", 1, false, true).is_ok());

    // フリーズ時点と確定直前で内容が一致（並行書き込みなし）→ 確認される。
    const std::string seen = "ユーザーが確認した内容\n";
    ReviewTarget t = target("a.md", seen, 2);
    t.freeze_hash = pika::util::xxh3_64_lf_hex(seen);

    AllConfirmedResult r = flow.confirm_all(index, {t}, "batch-3", 10);
    ASSERT_EQ(r.confirmed.size(), 1u);
    EXPECT_TRUE(r.skipped.empty());
    auto a = store.restore_baseline(index, "a.md");
    ASSERT_TRUE(a.is_ok());
    EXPECT_EQ(a.value(), seen);
}

TEST_F(ReviewFlowTest, RevertAllUndoesBatchBaselineReplace)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "a.md", "旧A\n", 1, false, true).is_ok());
    ASSERT_TRUE(store.set_baseline(index, "b.md", "旧B\n", 1, false, true).is_ok());

    AllConfirmedResult r = flow.confirm_all(
        index, {target("a.md", "新A\n", 2), target("b.md", "新B\n", 2)}, "batch-9", 10);
    ASSERT_EQ(r.confirmed.size(), 2u);

    // 一括取消で同一バッチの baseline-replace 退避がすべて除去される。
    const std::size_t reverted = flow.revert_all(index, "batch-9");
    EXPECT_EQ(reverted, 2u);
    for (const auto& e : index.entries)
    {
        for (const auto& s : e.stash)
        {
            EXPECT_NE(s.batch_id, "batch-9");
        }
    }
}

TEST_F(ReviewFlowTest, SetBaselinePutFailureLeavesEntryUnmutated)
{
    // set_baseline は内容 object 確保(put)が成功してから entry を一括コミットする（原子性）。put が
    // I/O 失敗しても entry を半端変異させない＝確認前の状態を完全に保つ（confirm/confirm_all の
    // 「失敗時は旧状態維持・未読維持」契約の土台。設計原則1）。objects
    // ディレクトリ位置を通常ファイル にして put
    // を確実に失敗させる（ディスク満杯・権限喪失の代替）。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;

    const std::string objects_dir = store.objects().dir();
    fs::create_directories(fs::path(objects_dir).parent_path());
    ASSERT_TRUE(pika::util::write_atomic(objects_dir, std::string("blocker")).is_ok());

    auto r = store.set_baseline(index, "a.md", "新内容\n", 5, false, true);
    EXPECT_TRUE(r.is_err());                // put 失敗が伝播
    EXPECT_EQ(index.find("a.md"), nullptr); // entry は作られない（半端変異なし）
    EXPECT_TRUE(index.entries.empty());
}

TEST_F(ReviewFlowTest, RevertAllRestoresOldBaselineContent)
{
    // 「すべて確認済み」の一括取消は、単に baseline-replace 退避を消すのではなく、退避が保持する旧
    // ベースライン内容へ実際に復元する（要件8.3「ワンクリックで一括取り消し」。設計原則1）。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "a.md", "旧A\n", 1, false, true).is_ok());
    ASSERT_TRUE(store.set_baseline(index, "b.md", "旧B\n", 1, false, true).is_ok());

    AllConfirmedResult r = flow.confirm_all(
        index, {target("a.md", "新A\n", 2), target("b.md", "新B\n", 2)}, "batch-R", 10);
    ASSERT_EQ(r.confirmed.size(), 2u);
    ASSERT_EQ(store.restore_baseline(index, "a.md").value(), "新A\n"); // 更新後は新内容

    const std::size_t reverted = flow.revert_all(index, "batch-R");
    EXPECT_EQ(reverted, 2u);

    auto a = store.restore_baseline(index, "a.md");
    auto b = store.restore_baseline(index, "b.md");
    ASSERT_TRUE(a.is_ok());
    ASSERT_TRUE(b.is_ok());
    EXPECT_EQ(a.value(), "旧A\n"); // 取消で旧内容へ戻る（復元点が破棄されていない）
    EXPECT_EQ(b.value(), "旧B\n");
    EXPECT_EQ(index.find("a.md")->baseline_hash, pika::util::xxh3_64_lf_hex("旧A\n"));
    EXPECT_TRUE(index.find("a.md")->unread); // 確認の取消なので未読へ戻る
}

TEST_F(ReviewFlowTest, ConfirmAllSkipsWhenOldBaselineCannotBeStashed)
{
    // 旧ベースラインを baseline-replace 退避へ保存できないとき、退避失敗を黙殺してベースラインを
    // 置き換えると revert_all（一括取消）で旧内容へ戻せなくなる。confirm_all は更新せずスキップ＝
    // 未読のまま残し、取消保証を死守する（sprint10 high / 設計原則1「データを失わない」）。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    ReviewFlow flow(store);
    SnapshotIndex index;

    const std::string old_baseline = "旧ベースライン\n";
    ASSERT_TRUE(store.set_baseline(index, "a.md", old_baseline, 1, false, true).is_ok());
    const std::string old_hash = index.find("a.md")->baseline_hash;
    ASSERT_FALSE(index.find("a.md")->baseline_object.empty());

    // 旧ベースラインの内容 object を物理削除し、退避の起点（旧内容の取り出し）を失敗させる。
    for (const auto& h : store.objects().list_objects())
    {
        store.objects().remove(h);
    }

    AllConfirmedResult r =
        flow.confirm_all(index, {target("a.md", "新しく確認した内容\n", 2)}, "batch-Z", 500);

    // 更新されず未読のまま（スキップ）。
    EXPECT_TRUE(r.confirmed.empty());
    ASSERT_EQ(r.skipped.size(), 1u);
    EXPECT_EQ(r.skipped[0], "a.md");

    // ベースラインは置き換えられていない（旧ハッシュのまま＝取消保証を破らない）。
    const auto* e = index.find("a.md");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->baseline_hash, old_hash);
    // baseline-replace 退避も積まれていない。
    for (const auto& s : e->stash)
    {
        EXPECT_NE(s.kind, StashKind::BaselineReplace);
    }
}

} // namespace
