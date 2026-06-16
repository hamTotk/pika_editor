// controller/document_controller の検証（sprint6 must）。
// - 確認済みフロー（confirm/confirm_all/rollback）が未読集合からの除去・マーク解除をするか。
// - 退避結合の Result を握り潰さない：退避失敗時はベースラインを更新せず未読を維持しエラー/skip
// を返す
//   （『データを失わない』最上位原則の UI 側ガード。前回 report 持ち越し #5）。
// - 保存フロー（prepare_save）：表現可能性チェック・衝突検知・incoming
// 退避・退避不能ブロックの判定。
#include "controller/document_controller.h"

#include "core/snapshot/snapshot_store.h"
#include "util/atomic_file.h"
#include "util/encoding.h"
#include "util/hash.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace
{

namespace fs = std::filesystem;

using pika::controller::ConfirmAllOutcome;
using pika::controller::ConfirmOutcome;
using pika::controller::DocumentController;
using pika::controller::SaveContext;
using pika::controller::SaveDecision;
using pika::controller::SavePlan;
using pika::core::document::FileContentClass;
using pika::core::document::ReviewTarget;
using pika::core::snapshot::SnapshotIndex;
using pika::core::snapshot::SnapshotStore;
using pika::core::snapshot::workspace_key;
using pika::core::workspace::UnreadSet;
using pika::util::Encoding;
using pika::util::ErrorCode;

class DocumentControllerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root_ = fs::temp_directory_path() /
                ("pika_doccontroller_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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
    c.content_object_allowed = false; // 10MB以上・画像＝内容 object を持てない（退避不能）
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

// ---- 確認済みにする：ベースライン更新＋未読解除 ----------------------------

TEST_F(DocumentControllerTest, ConfirmClearsUnreadAndUpdatesBaseline)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    DocumentController doc(store);
    SnapshotIndex index;
    UnreadSet unread;
    unread.mark("a.md");
    ASSERT_TRUE(unread.is_unread("a.md"));

    auto out = doc.confirm(index, unread, target("a.md", "確認した内容\n", 5));
    ASSERT_TRUE(out.is_ok());
    EXPECT_TRUE(out.value().unread_cleared);
    EXPECT_TRUE(out.value().baseline_updated);
    // 未読集合から外れ、ツリー/タブのマーク解除の素材になる（design 5.4）。
    EXPECT_FALSE(unread.is_unread("a.md"));
    // ベースラインが現内容で更新されている。
    const auto* e = index.find("a.md");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->baseline_hash, pika::util::xxh3_64_lf_hex("確認した内容\n"));
    EXPECT_FALSE(e->unread);
}

TEST_F(DocumentControllerTest, ConfirmFailureKeepsUnreadAndReturnsError)
{
    // ベースライン更新（set_baseline の内容 object put）が I/O 失敗したとき、Result を握り潰さず
    // エラーを返し、未読を維持する（確認できていない＝未読のまま。データを失わない）。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    DocumentController doc(store);
    SnapshotIndex index;
    UnreadSet unread;
    unread.mark("a.md");

    // objects ディレクトリ位置を通常ファイルにして put を確実に失敗させる（権限喪失・満杯の代替）。
    const std::string objects_dir = store.objects().dir();
    fs::create_directories(fs::path(objects_dir).parent_path());
    ASSERT_TRUE(pika::util::write_atomic(objects_dir, std::string("blocker")).is_ok());

    auto out = doc.confirm(index, unread, target("a.md", "確認した内容\n", 5));
    EXPECT_TRUE(out.is_err());              // Result を握り潰さずエラーを伝播
    EXPECT_TRUE(unread.is_unread("a.md"));  // 未読を維持（確認失敗＝未確認のまま）
    EXPECT_EQ(index.find("a.md"), nullptr); // ベースラインは作られない（半端変異なし）
}

TEST_F(DocumentControllerTest, ConfirmLargeOrImageUpdatesHashBaselineAndClearsUnread)
{
    // 内容を持たない（10MB以上・画像）ファイルもハッシュベースラインのみ更新し未読解除できる（D3）。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    DocumentController doc(store);
    SnapshotIndex index;
    UnreadSet unread;
    unread.mark("big.bin");

    auto out =
        doc.confirm(index, unread, target("big.bin", "巨大ファイルの内容\n", 9, large_or_image()));
    ASSERT_TRUE(out.is_ok());
    EXPECT_FALSE(unread.is_unread("big.bin"));
    const auto* e = index.find("big.bin");
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->baseline_object.empty()); // 内容 object は持たない（ハッシュのみ）
    EXPECT_FALSE(e->baseline_hash.empty());
}

// ---- すべて確認済み：confirmed のみ未読解除・skipped は未読維持 --------------

TEST_F(DocumentControllerTest, ConfirmAllClearsConfirmedUnreadKeepsSkipped)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    DocumentController doc(store);
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "a.md", "旧A\n", 1, false, true).is_ok());
    ASSERT_TRUE(store.set_baseline(index, "b.md", "旧B\n", 1, false, true).is_ok());

    UnreadSet unread;
    unread.mark("a.md");
    unread.mark("b.md");

    // b.md は freeze_hash と現内容を不一致にして並行変化スキップさせる。
    ReviewTarget ta = target("a.md", "新A\n", 2);
    ta.freeze_hash = pika::util::xxh3_64_lf_hex("新A\n");
    ReviewTarget tb = target("b.md", "新B\n", 2);
    tb.freeze_hash = pika::util::xxh3_64_lf_hex("フリーズ時とは違う内容\n");

    ConfirmAllOutcome out = doc.confirm_all(index, unread, {ta, tb}, "batch-1", 10);
    ASSERT_EQ(out.confirmed.size(), 1u);
    EXPECT_EQ(out.confirmed[0], "a.md");
    ASSERT_EQ(out.skipped.size(), 1u);
    EXPECT_EQ(out.skipped[0], "b.md");

    EXPECT_FALSE(unread.is_unread("a.md")); // 確認できたファイルは未読解除
    EXPECT_TRUE(unread.is_unread("b.md"));  // 並行変化でスキップ＝未読維持
}

TEST_F(DocumentControllerTest, ConfirmAllSkipsAndKeepsUnreadWhenStashFails)
{
    // 旧ベースラインを baseline-replace 退避に保存できないとき、confirm_all
    // は更新せずスキップ＝未読の
    // まま残す（退避結合の失敗を握り潰さず未読維持。一括取消保証を死守する。設計原則1）。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    DocumentController doc(store);
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "a.md", "旧A\n", 1, false, true).is_ok());

    UnreadSet unread;
    unread.mark("a.md");

    // 旧ベースラインの内容 object を物理削除し、退避の起点を失敗させる。
    for (const auto& h : store.objects().list_objects())
    {
        store.objects().remove(h);
    }

    ConfirmAllOutcome out =
        doc.confirm_all(index, unread, {target("a.md", "新A\n", 2)}, "batch-Z", 10);
    EXPECT_TRUE(out.confirmed.empty());
    ASSERT_EQ(out.skipped.size(), 1u);
    EXPECT_EQ(out.skipped[0], "a.md");
    EXPECT_TRUE(unread.is_unread("a.md")); // 退避失敗＝未読維持（データを失わない）
    // ベースラインは旧内容のまま（取消保証を破らない）。
    EXPECT_EQ(index.find("a.md")->baseline_hash, pika::util::xxh3_64_lf_hex("旧A\n"));
}

// ---- 一括取消：取り消したファイルを未読へ戻す ------------------------------

TEST_F(DocumentControllerTest, RevertAllRestoresBaselineAndMarksUnread)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    DocumentController doc(store);
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "a.md", "旧A\n", 1, false, true).is_ok());

    UnreadSet unread;
    unread.mark("a.md");
    ConfirmAllOutcome out =
        doc.confirm_all(index, unread, {target("a.md", "新A\n", 2)}, "batch-R", 10);
    ASSERT_EQ(out.confirmed.size(), 1u);
    ASSERT_FALSE(unread.is_unread("a.md")); // 確認で未読解除

    const std::size_t reverted = doc.revert_all(index, unread, "batch-R", out.confirmed);
    EXPECT_EQ(reverted, 1u);
    // 取消で旧内容へ戻り、再び未読になる（ユーザーが確認を取り消した）。
    EXPECT_EQ(store.restore_baseline(index, "a.md").value(), "旧A\n");
    EXPECT_TRUE(unread.is_unread("a.md"));
}

// ---- 巻き戻し：退避→ベースライン内容を返す・退避不能は Unsupported ----------

TEST_F(DocumentControllerTest, RollbackStashesCurrentAndReturnsBaseline)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    DocumentController doc(store);
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "a.md", "ベースライン\n", 1, false, true).is_ok());

    EXPECT_TRUE(doc.can_rollback(index, "a.md", stashable()));
    auto rolled = doc.rollback(index, target("a.md", "現在内容\n", 3));
    ASSERT_TRUE(rolled.is_ok());
    EXPECT_EQ(rolled.value(), "ベースライン\n"); // 巻き戻し先＝ベースライン
}

TEST_F(DocumentControllerTest, RollbackBlockedForUnstashableReturnsUnsupported)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    DocumentController doc(store);
    SnapshotIndex index;

    EXPECT_FALSE(doc.can_rollback(index, "big.bin", large_or_image()));
    auto rolled = doc.rollback(index, target("big.bin", "現在内容\n", 3, large_or_image()));
    EXPECT_TRUE(rolled.is_err()); // Result を握り潰さず Unsupported を返す
    EXPECT_EQ(rolled.code(), ErrorCode::Unsupported);
}

// ---- 保存フロー：表現可能性・衝突検知・incoming 退避・退避不能ブロック ------

TEST_F(DocumentControllerTest, SaveProceedsWhenNoConflict)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    DocumentController doc(store);
    SnapshotIndex index;

    SaveContext ctx;
    ctx.rel_path = "a.md";
    ctx.buffer_content = "編集後の内容\n";
    ctx.disk_content = "最後に読んだ内容\n";
    ctx.last_loaded_hash =
        pika::util::xxh3_64_lf_hex("最後に読んだ内容\n"); // ディスクと一致＝衝突なし
    ctx.encoding = Encoding::Utf8;

    SavePlan plan = doc.prepare_save(index, ctx);
    EXPECT_TRUE(plan.ok());
    EXPECT_FALSE(plan.conflict);
    EXPECT_TRUE(plan.stash_hash.empty()); // 衝突なし＝退避しない
}

TEST_F(DocumentControllerTest, SaveDetectsConflictAndStashesIncoming)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    DocumentController doc(store);
    SnapshotIndex index;

    SaveContext ctx;
    ctx.rel_path = "a.md";
    ctx.buffer_content = "編集後の内容\n";
    // ディスクが最後の読み込み時点から外部に書き換えられている（衝突＝incoming）。
    ctx.disk_content = "外部に書き換えられた内容\n";
    ctx.last_loaded_hash = pika::util::xxh3_64_lf_hex("最後に読んだ内容\n");
    ctx.encoding = Encoding::Utf8;
    ctx.time = 100;

    SavePlan plan = doc.prepare_save(index, ctx);
    EXPECT_TRUE(plan.ok()); // 退避できたので上書きしてよい
    EXPECT_TRUE(plan.conflict);
    ASSERT_FALSE(plan.stash_hash.empty());
    // incoming 退避に外部内容が保存され、復元で原内容と一致する（要件7.3。失われない）。
    auto restored = store.restore_stash(plan.stash_hash);
    ASSERT_TRUE(restored.is_ok());
    EXPECT_EQ(restored.value(), "外部に書き換えられた内容\n");
}

TEST_F(DocumentControllerTest, SaveBlockedWhenConflictAndUnstashable)
{
    // 衝突したが退避を取れない（10MB以上・画像・機密）＝既定でブロックする（要件7.3。上書きしない）。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    DocumentController doc(store);
    SnapshotIndex index;

    SaveContext ctx;
    ctx.rel_path = "big.bin";
    ctx.buffer_content = "編集後\n";
    ctx.disk_content = "外部変更\n";
    ctx.last_loaded_hash = pika::util::xxh3_64_lf_hex("元の内容\n");
    ctx.cls = large_or_image();

    SavePlan plan = doc.prepare_save(index, ctx);
    EXPECT_FALSE(plan.ok());
    EXPECT_EQ(plan.decision, SaveDecision::BlockedUnstashable);
    EXPECT_TRUE(plan.conflict);
    EXPECT_TRUE(plan.stash_hash.empty()); // 退避していない
    EXPECT_EQ(plan.error.code, ErrorCode::Unsupported);
}

TEST_F(DocumentControllerTest, SaveBlockedWhenContentNotRepresentable)
{
    // Shift_JIS
    // で表現できない文字（絵文字）を含むと保存を中断する（要件5.2・G2。別バイトに化けさせない）。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    DocumentController doc(store);
    SnapshotIndex index;

    SaveContext ctx;
    ctx.rel_path = "a.txt";
    ctx.buffer_content = "絵文字を含む😀\n"; // CP932 に真のマッピングなし
    ctx.disk_content = "絵文字を含む😀\n";
    ctx.last_loaded_hash = pika::util::xxh3_64_lf_hex("絵文字を含む😀\n");
    ctx.encoding = Encoding::ShiftJis;

    SavePlan plan = doc.prepare_save(index, ctx);
    EXPECT_FALSE(plan.ok());
    EXPECT_EQ(plan.decision, SaveDecision::BlockedEncoding);
    EXPECT_EQ(plan.error.code, ErrorCode::Encoding);
}

TEST_F(DocumentControllerTest, SaveStashFailureBlocksOverwrite)
{
    // 衝突して退避を試みたが退避 I/O に失敗したら上書きをブロックする（外部変更を失わない）。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    DocumentController doc(store);
    SnapshotIndex index;

    // objects 位置を通常ファイルにして put_stash を確実に失敗させる。
    const std::string objects_dir = store.objects().dir();
    fs::create_directories(fs::path(objects_dir).parent_path());
    ASSERT_TRUE(pika::util::write_atomic(objects_dir, std::string("blocker")).is_ok());

    SaveContext ctx;
    ctx.rel_path = "a.md";
    ctx.buffer_content = "編集後\n";
    ctx.disk_content = "外部変更\n";
    ctx.last_loaded_hash = pika::util::xxh3_64_lf_hex("元の内容\n");
    ctx.encoding = Encoding::Utf8;

    SavePlan plan = doc.prepare_save(index, ctx);
    EXPECT_FALSE(plan.ok());
    EXPECT_EQ(plan.decision, SaveDecision::BlockedStashFailed);
    EXPECT_TRUE(plan.conflict);
}

} // namespace
