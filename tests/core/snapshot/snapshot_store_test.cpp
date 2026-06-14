// core/snapshot SnapshotStore の検証（sprint5 must の中核）。
//  - 保存→復元の同一性（zstd 圧縮 object・内容ハッシュ名）
//  - 退避フロー（conflict/incoming/rollback/baseline-replace）の保存・復元
//  - index破損復元（objects 走査から復元待ち退避一覧）
//  - 機密ファイルは内容を保存せず baselineHash のみ記録
//  - 容量管理（ファイルごと最新10件 LRU・容量GC500MB・90日GC・未復元14日保護）
//  - object の物理削除が mark-and-sweep で共有実体を誤削除しない
// すべてテンポラリのデータルートを使った実 FS で観測する（design.md 13章 / 要件9章）。
#include "core/snapshot/snapshot_store.h"

#include "core/snapshot/compression.h"
#include "core/snapshot/index_io.h"
#include "core/snapshot/json_lite.h"
#include "core/snapshot/object_store.h"
#include "core/snapshot/snapshot_types.h"
#include "util/atomic_file.h"

#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace
{

namespace fs = std::filesystem;

using pika::core::snapshot::CapacityPolicy;
using pika::core::snapshot::file_key;
using pika::core::snapshot::IndexEntry;
using pika::core::snapshot::SnapshotIndex;
using pika::core::snapshot::SnapshotStore;
using pika::core::snapshot::StashKind;
using pika::core::snapshot::StashRequest;
using pika::core::snapshot::workspace_key;
using pika::util::ErrorCode;

class SnapshotStoreTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root_ = fs::temp_directory_path() /
                ("pika_snapstore_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

StashRequest stash_req(const std::string& rel, StashKind kind, const std::string& content,
                       const std::string& batch = "")
{
    StashRequest r;
    r.rel_path = rel;
    r.kind = kind;
    r.content = content;
    r.batch_id = batch;
    return r;
}

// --- 保存→復元の同一性 -------------------------------------------------------

TEST_F(SnapshotStoreTest, BaselineSaveRestoreIdentity)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;

    const std::string content = "# タイトル\n本文\r\n混在改行\n";
    auto set = store.set_baseline(index, "a.md", content, 1700000000, false, true);
    ASSERT_TRUE(set.is_ok());
    EXPECT_FALSE(set.value().baseline_object.empty());
    EXPECT_FALSE(set.value().unread); // ベースライン更新で未読解除

    auto restored = store.restore_baseline(index, "a.md");
    ASSERT_TRUE(restored.is_ok());
    EXPECT_EQ(restored.value(), content);
}

TEST_F(SnapshotStoreTest, BaselineRoundTripsAfterPersist)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    const std::string content = "persisted baseline body\n";
    ASSERT_TRUE(store.set_baseline(index, "p.txt", content, 1, false, true).is_ok());
    ASSERT_TRUE(store.save(index).is_ok());

    // 別インスタンスで読み直しても object から復元できる。
    SnapshotStore reopened(snapshots_root(), workspace_key("C:/ws"));
    auto loaded = reopened.load();
    ASSERT_TRUE(loaded.is_ok());
    auto restored = reopened.restore_baseline(loaded.value(), "p.txt");
    ASSERT_TRUE(restored.is_ok());
    EXPECT_EQ(restored.value(), content);
}

// --- 退避フロー（4種別） -----------------------------------------------------

TEST_F(SnapshotStoreTest, AllStashKindsSaveAndRestore)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;

    struct Case
    {
        StashKind kind;
        std::string content;
        std::string batch;
    };
    const Case cases[] = {
        {StashKind::Conflict, "my unsaved edit", ""},
        {StashKind::Incoming, "external disk content", ""},
        {StashKind::Rollback, "content before rollback", ""},
        {StashKind::BaselineReplace, "old baseline content", "batch-1"},
    };

    for (const auto& c : cases)
    {
        auto added = store.add_stash(index, stash_req("f.md", c.kind, c.content, c.batch), 1000);
        ASSERT_TRUE(added.is_ok()) << pika::core::snapshot::to_string(c.kind);
        auto restored = store.restore_stash(added.value().hash);
        ASSERT_TRUE(restored.is_ok());
        EXPECT_EQ(restored.value(), c.content) << pika::core::snapshot::to_string(c.kind);
    }

    const IndexEntry* e = index.find("f.md");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->stash.size(), 4u);
}

// --- index 破損復元 ----------------------------------------------------------

TEST_F(SnapshotStoreTest, RecoversStashesAfterIndexCorruption)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    ASSERT_TRUE(
        store.add_stash(index, stash_req("a.md", StashKind::Conflict, "edit A"), 10).is_ok());
    ASSERT_TRUE(
        store.add_stash(index, stash_req("b.md", StashKind::Incoming, "incoming B"), 20).is_ok());
    ASSERT_TRUE(store.save(index).is_ok());

    // index.json を破損させる（退避 object・サイドカーはディスクに残る）。
    ASSERT_TRUE(pika::util::write_atomic(store.index_path(), "{ corrupt }").is_ok());

    // 通常の load は破損として失敗する。
    auto loaded = store.load();
    ASSERT_TRUE(loaded.is_err());
    EXPECT_EQ(loaded.code(), ErrorCode::Io);

    // 退避＝最後の砦：objects 走査から復元待ち一覧を提示でき、内容も復元できる（D1）。
    auto pending = store.recover_pending_stashes();
    ASSERT_EQ(pending.size(), 2u);
    bool a_ok = false;
    bool b_ok = false;
    for (const auto& r : pending)
    {
        auto body = store.restore_stash(r.object_hash);
        ASSERT_TRUE(body.is_ok());
        if (r.rel_path == "a.md")
        {
            a_ok = (body.value() == "edit A");
        }
        if (r.rel_path == "b.md")
        {
            b_ok = (body.value() == "incoming B");
        }
    }
    EXPECT_TRUE(a_ok);
    EXPECT_TRUE(b_ok);
}

// --- 機密ファイルはハッシュのみ ----------------------------------------------

TEST_F(SnapshotStoreTest, SensitiveFileRecordsHashOnly)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    const std::string secret = "SECRET_TOKEN=abc123\n";

    auto set = store.set_baseline(index, ".env", secret, 1, /*sensitive=*/true,
                                  /*content_object_allowed=*/true);
    ASSERT_TRUE(set.is_ok());
    EXPECT_FALSE(set.value().baseline_hash.empty());  // baselineHash は記録する
    EXPECT_TRUE(set.value().baseline_object.empty()); // 内容 object は持たない

    // 内容を持たないため復元（差分・巻き戻し）は非活性（Unsupported）。
    auto restored = store.restore_baseline(index, ".env");
    ASSERT_TRUE(restored.is_err());
    EXPECT_EQ(restored.code(), ErrorCode::Unsupported);

    // objects に平文が一切書かれていない（データ最小化）。
    EXPECT_TRUE(store.objects().list_objects().empty());
}

TEST_F(SnapshotStoreTest, LargeFileRecordsHashOnly)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    // 10MB 以上・画像相当は content_object_allowed=false（内容 object を持たない）。
    auto set = store.set_baseline(index, "big.bin", "huge content marker", 1, false,
                                  /*content_object_allowed=*/false);
    ASSERT_TRUE(set.is_ok());
    EXPECT_FALSE(set.value().baseline_hash.empty());
    EXPECT_TRUE(set.value().baseline_object.empty());
    EXPECT_TRUE(store.objects().list_objects().empty());
}

// --- 容量管理：ファイルごと最新10件 LRU --------------------------------------

TEST_F(SnapshotStoreTest, PerFileStashLruKeepsLatestTen)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    const std::int64_t day = 24 * 3600;
    const std::int64_t now = 1000LL * day;
    // 15 件を 30 日間隔で追加する。最新の退避時刻（= now）を保護判定の基準時とするため、
    // 14日保護の窓に入るのは最新 1 件のみで、残り 14 件は非保護＝LRU で落とせる。
    // LRU は非保護退避を10件枠に収めるため、最新10件が残り、古い 5 件が落ちる。
    // （全件が直近＝全件保護のケースは AddStashLruKeepsProtectedBeyondTen が別途検証する。）
    for (int i = 0; i < 15; ++i)
    {
        const std::string body = "edit " + std::to_string(i);
        const std::int64_t t = now - (14 - i) * 30 * day; // 最古 = now-420日, 最新 = now
        ASSERT_TRUE(
            store.add_stash(index, stash_req("f.md", StashKind::Conflict, body), t).is_ok());
    }
    const IndexEntry* e = index.find("f.md");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->stash.size(), 10u);
    // 残るのは新しい側（最新10件）。古い 5 件（i=0..4）は非保護で落ちている。
    const std::int64_t cutoff = now - (14 - 5) * 30 * day; // i>=5 の最古
    for (const auto& s : e->stash)
    {
        EXPECT_GE(s.time, cutoff);
    }
}

TEST_F(SnapshotStoreTest, BaselineReplaceNotCountedInPerFileLru)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    const std::int64_t day = 24 * 3600;
    const std::int64_t now = 1000LL * day;
    // baseline-replace は別バッチ枠。10件枠を圧迫しない。
    ASSERT_TRUE(store
                    .add_stash(index, stash_req("f.md", StashKind::BaselineReplace, "old", "b1"),
                               now - 200 * day)
                    .is_ok());
    // 12 件の Conflict を 30 日間隔で追加（最新 1 件のみ14日保護・残り11件は非保護）。
    // LRU は非バッチ退避だけを10件枠で数え baseline-replace は枠外なので、合計11件になる。
    for (int i = 0; i < 12; ++i)
    {
        const std::int64_t t = now - (11 - i) * 30 * day;
        ASSERT_TRUE(store
                        .add_stash(index,
                                   stash_req("f.md", StashKind::Conflict, "e" + std::to_string(i)),
                                   t)
                        .is_ok());
    }
    const IndexEntry* e = index.find("f.md");
    ASSERT_NE(e, nullptr);
    // 非バッチ 10 件 ＋ baseline-replace 1 件 = 11。
    EXPECT_EQ(e->stash.size(), 11u);
    int batch_count = 0;
    for (const auto& s : e->stash)
    {
        if (s.kind == StashKind::BaselineReplace)
        {
            ++batch_count;
        }
    }
    EXPECT_EQ(batch_count, 1);
}

// --- 容量管理：LRU × 14日保護の相互作用 --------------------------------------

// 未復元かつ14日以内の退避は、ファイルごと最新10件 LRU の枠を超えても保護される（要件9.3）。
// add_stash のインライン LRU 経路で保護を侵さないことを観測する（退避＝最後の砦の決定的検証）。
TEST_F(SnapshotStoreTest, AddStashLruKeepsProtectedBeyondTen)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    const std::int64_t day = 24 * 3600;
    const std::int64_t base = 1000LL * day;

    // 11 件すべて「未復元かつ生成時刻が直近（互いに数秒差）」＝全件 14日以内で保護対象。
    // add_stash は新しい退避の time を保護判定の基準時とするため、過去 11 件は base..base+10。
    for (int i = 0; i < 11; ++i)
    {
        const std::string body = "protected " + std::to_string(i);
        ASSERT_TRUE(
            store.add_stash(index, stash_req("f.md", StashKind::Conflict, body), base + i).is_ok());
    }
    const IndexEntry* e = index.find("f.md");
    ASSERT_NE(e, nullptr);
    // 全 11 件が未復元・14日以内で保護されるため、10件枠を超えても 1 件も落ちない。
    EXPECT_EQ(e->stash.size(), 11u);
    // 最古（protected 0）も残っており内容を復元できる（黙って消えていない）。
    auto oldest = store.restore_stash(e->stash.front().hash);
    ASSERT_TRUE(oldest.is_ok());
    EXPECT_EQ(oldest.value(), "protected 0");
}

// 非保護（古い／復元済み）退避は LRU で落ち、保護退避だけが10件枠を超えて残る混在ケース。
TEST_F(SnapshotStoreTest, AddStashLruDropsUnprotectedButKeepsProtected)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    const std::int64_t day = 24 * 3600;
    const std::int64_t now = 1000LL * day;

    // まず 14日より古い（非保護）退避を 5 件積む。
    for (int i = 0; i < 5; ++i)
    {
        ASSERT_TRUE(
            store
                .add_stash(index, stash_req("f.md", StashKind::Conflict, "old" + std::to_string(i)),
                           now - (100 - i) * day)
                .is_ok());
    }
    // 続いて直近（保護対象）退避を 8 件積む。合計 13 件＝10件枠を 3 件超過。
    // add_stash の基準時は最後に積む退避の time（= now 近傍）なので、古い 5 件は非保護のまま。
    for (int i = 0; i < 8; ++i)
    {
        ASSERT_TRUE(
            store
                .add_stash(index, stash_req("f.md", StashKind::Conflict, "new" + std::to_string(i)),
                           now - 8 + i)
                .is_ok());
    }

    const IndexEntry* e = index.find("f.md");
    ASSERT_NE(e, nullptr);
    // 保護退避 8 件は枠超過でも残る。非保護の古い 5 件のうち、枠（10件）に収めるため 3 件が落ち、
    // 2 件が残る（古い側から削除。保護分 8 ＋ 非保護残 2 = 10）。
    EXPECT_EQ(e->stash.size(), 10u);
    int protected_count = 0;
    for (const auto& s : e->stash)
    {
        if (!s.restored && (now - s.time) <= 14 * day)
        {
            ++protected_count;
        }
    }
    EXPECT_EQ(protected_count, 8); // 保護退避は 1 件も失われない
}

// enforce_capacity の LRU 経路も同じく保護退避を侵さない（add_stash と二経路で乖離しない）。
TEST_F(SnapshotStoreTest, EnforceCapacityLruKeepsProtectedBeyondTen)
{
    // per_file_stash_limit を維持したまま、保護退避を11件積んでから enforce_capacity を呼ぶ。
    // ここでは add_stash の LRU を介さず一括投入したいので policy で枠を一時的に広げて積み、
    // enforce_capacity の LRU 単独挙動を観測する。
    CapacityPolicy build_policy;
    build_policy.per_file_stash_limit = 100; // 投入時は LRU を効かせない
    SnapshotStore builder(snapshots_root(), workspace_key("C:/ws"), build_policy);
    SnapshotIndex index;
    const std::int64_t day = 24 * 3600;
    const std::int64_t now = 1000LL * day;
    for (int i = 0; i < 11; ++i)
    {
        ASSERT_TRUE(builder
                        .add_stash(index,
                                   stash_req("f.md", StashKind::Conflict, "p" + std::to_string(i)),
                                   now - i) // すべて直近＝保護対象
                        .is_ok());
    }
    ASSERT_EQ(index.find("f.md")->stash.size(), 11u);

    // 既定枠（10件）の store で enforce_capacity を実行しても、保護退避11件は削られない。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws")); // per_file_stash_limit=10
    store.enforce_capacity(index, now, now);                       // 90日GC 不発・容量GC 不発
    EXPECT_EQ(index.find("f.md")->stash.size(), 11u);
}

// --- 容量管理：90日GC ＋ 14日保護 --------------------------------------------

TEST_F(SnapshotStoreTest, AgeGcRemovesOldButProtectsRecentUnrestored)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    const std::int64_t day = 24 * 3600;
    const std::int64_t now = 1000LL * day;

    // 古い退避（100日前・未復元）と最近の退避（5日前・未復元）。
    ASSERT_TRUE(
        store.add_stash(index, stash_req("f.md", StashKind::Conflict, "old"), now - 100 * day)
            .is_ok());
    ASSERT_TRUE(
        store.add_stash(index, stash_req("f.md", StashKind::Conflict, "recent"), now - 5 * day)
            .is_ok());

    // last_opened を 100 日前にして 90日GC を発火させる。
    store.enforce_capacity(index, now, now - 100 * day);

    const IndexEntry* e = index.find("f.md");
    ASSERT_NE(e, nullptr);
    // 古い退避は削除、最近（14日以内・未復元）の退避は保護されて残る。
    ASSERT_EQ(e->stash.size(), 1u);
    auto body = store.restore_stash(e->stash[0].hash);
    ASSERT_TRUE(body.is_ok());
    EXPECT_EQ(body.value(), "recent");
}

TEST_F(SnapshotStoreTest, AgeGcRemovesRestoredEvenIfRecent)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    const std::int64_t day = 24 * 3600;
    const std::int64_t now = 1000LL * day;

    ASSERT_TRUE(
        store.add_stash(index, stash_req("f.md", StashKind::Conflict, "restored body"), now - day)
            .is_ok());
    // 復元済みにマークする（保護は「未復元かつ14日以内」が条件。復元済みは保護されない）。
    IndexEntry* e = index.find("f.md");
    ASSERT_NE(e, nullptr);
    e->stash[0].restored = true;

    store.enforce_capacity(index, now, now - 100 * day); // 90日GC 発火
    EXPECT_TRUE(index.find("f.md")->stash.empty());
}

// --- 容量管理：容量GC500MB（保護を侵さない） --------------------------------

TEST_F(SnapshotStoreTest, ByteGcEvictsOldestUnprotectedButKeepsProtected)
{
    CapacityPolicy policy;
    policy.per_file_stash_limit = 100; // LRU を無効化して容量GC を単独で観測
    policy.total_byte_limit = 1;       // 1 バイトでも超過扱いにして容量GC を強制発火
    policy.protect_seconds = 14 * 24 * 3600;
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"), policy);
    SnapshotIndex index;
    const std::int64_t day = 24 * 3600;
    const std::int64_t now = 1000LL * day;

    // 保護対象（最近・未復元）と非保護（復元済み）。
    ASSERT_TRUE(
        store
            .add_stash(index, stash_req("f.md", StashKind::Conflict, "protected recent"), now - day)
            .is_ok());
    ASSERT_TRUE(
        store.add_stash(index, stash_req("g.md", StashKind::Conflict, "evictable"), now - 2 * day)
            .is_ok());
    index.find("g.md")->stash[0].restored = true; // 非保護にする

    store.enforce_capacity(index, now, now); // last_opened=now で 90日GC は不発、容量GC のみ

    // 非保護（evictable）は容量超過で削除され、保護退避は上限超過でも残る。
    EXPECT_TRUE(index.find("g.md")->stash.empty());
    ASSERT_EQ(index.find("f.md")->stash.size(), 1u);
}

// --- mark-and-sweep：共有実体の誤削除防止 ------------------------------------

TEST_F(SnapshotStoreTest, SweepKeepsSharedObjectReferencedElsewhere)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;

    // 同一内容を 2 ファイルが退避 → object は 1 つ共有（重複排除）。
    const std::string shared = "shared content body";
    auto a = store.add_stash(index, stash_req("a.md", StashKind::Conflict, shared), 1);
    auto b = store.add_stash(index, stash_req("b.md", StashKind::Conflict, shared), 2);
    ASSERT_TRUE(a.is_ok());
    ASSERT_TRUE(b.is_ok());
    EXPECT_EQ(a.value().hash, b.value().hash);
    EXPECT_EQ(store.objects().list_objects().size(), 1u);

    // a.md の退避だけを index から外して sweep。b.md がまだ参照するため object は消えない。
    IndexEntry* ea = index.find("a.md");
    ea->stash.clear();
    store.sweep_unreferenced_objects(index);
    EXPECT_TRUE(store.objects().contains(a.value().hash));

    // b.md の参照も外すと、どこからも参照されず物理削除される。
    index.find("b.md")->stash.clear();
    const std::size_t removed = store.sweep_unreferenced_objects(index);
    EXPECT_EQ(removed, 1u);
    EXPECT_FALSE(store.objects().contains(a.value().hash));
}

TEST_F(SnapshotStoreTest, SweepKeepsBaselineObjectSharedWithStash)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    const std::string content = "baseline equals stash content";

    // ベースラインと退避が同一内容 → object 共有。
    ASSERT_TRUE(store.set_baseline(index, "a.md", content, 1, false, true).is_ok());
    auto st = store.add_stash(index, stash_req("a.md", StashKind::Rollback, content), 2);
    ASSERT_TRUE(st.is_ok());
    EXPECT_EQ(store.objects().list_objects().size(), 1u);

    // 退避を外して sweep してもベースラインが参照するため object は残る（誤削除しない。D5）。
    index.find("a.md")->stash.clear();
    store.sweep_unreferenced_objects(index);
    auto restored = store.restore_baseline(index, "a.md");
    ASSERT_TRUE(restored.is_ok());
    EXPECT_EQ(restored.value(), content);
}

// --- batch 一括取消 ----------------------------------------------------------

TEST_F(SnapshotStoreTest, RevertBatchRemovesAllBaselineReplaceInBatch)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    // 同一バッチで複数ファイルの baseline-replace を退避（「すべて確認済み」相当）。
    ASSERT_TRUE(
        store.add_stash(index, stash_req("a.md", StashKind::BaselineReplace, "oldA", "B1"), 1)
            .is_ok());
    ASSERT_TRUE(
        store.add_stash(index, stash_req("b.md", StashKind::BaselineReplace, "oldB", "B1"), 2)
            .is_ok());
    // 別バッチの退避は取消対象外。
    ASSERT_TRUE(
        store.add_stash(index, stash_req("c.md", StashKind::BaselineReplace, "oldC", "B2"), 3)
            .is_ok());

    const std::size_t reverted = store.revert_batch(index, "B1");
    EXPECT_EQ(reverted, 2u);
    EXPECT_TRUE(index.find("a.md")->stash.empty());
    EXPECT_TRUE(index.find("b.md")->stash.empty());
    EXPECT_EQ(index.find("c.md")->stash.size(), 1u); // B2 は残る
}

// --- wsKey と purge ----------------------------------------------------------

TEST_F(SnapshotStoreTest, WorkspaceKeyAndFileKeyDiffer)
{
    EXPECT_NE(workspace_key("C:/a"), workspace_key("C:/b"));
    EXPECT_EQ(workspace_key("C:/a"), workspace_key("C:/a")); // 決定的
    // 単体ファイルキーは "file-" 接頭辞で衝突しない。
    EXPECT_EQ(file_key("C:/a").rfind("file-", 0), 0u);
    EXPECT_NE(file_key("C:/a"), workspace_key("C:/a"));
}

TEST_F(SnapshotStoreTest, PurgeRemovesAllSnapshotData)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "a.md", "body", 1, false, true).is_ok());
    ASSERT_TRUE(store.save(index).is_ok());
    ASSERT_TRUE(fs::exists(store.ws_dir()));

    store.purge();
    EXPECT_FALSE(fs::exists(store.ws_dir())); // index・objects ごと消える（要件9.4）
}

// --- ワークスペースを汚さない（should） --------------------------------------

TEST_F(SnapshotStoreTest, DoesNotWriteInsideWorkspaceFolder)
{
    // 退避・ベースラインの保存先はデータルート配下の snapshots\ のみ。
    // ワークスペース実体（別フォルダ）には一切書かないことを観測する（要件9.1 / should）。
    const fs::path workspace = root_ / "the_workspace";
    fs::create_directories(workspace);

    SnapshotStore store(snapshots_root(), workspace_key(workspace.string()));
    SnapshotIndex index;
    ASSERT_TRUE(store.set_baseline(index, "a.md", "content", 1, false, true).is_ok());
    ASSERT_TRUE(store.add_stash(index, stash_req("a.md", StashKind::Conflict, "edit"), 1).is_ok());
    ASSERT_TRUE(store.save(index).is_ok());

    // ワークスペースフォルダ内には pika の管理ファイルが何も作られていない（.pika 等を作らない）。
    int count = 0;
    for (const auto& e : fs::recursive_directory_iterator(workspace))
    {
        (void)e;
        ++count;
    }
    EXPECT_EQ(count, 0);
}

// --- セキュリティ：index.json 由来 hash のパストラバーサル多層防御 ------------

TEST_F(SnapshotStoreTest, ObjectStoreRejectsMalformedHash)
{
    using pika::core::snapshot::ObjectStore;
    // 妥当（XXH3-64 = 16 桁小文字 16 進数）のみ通す。
    EXPECT_TRUE(ObjectStore::is_valid_hash("0123456789abcdef"));
    // 区切り文字・".."・大文字・長さ違い・空はすべて拒否する。
    EXPECT_FALSE(ObjectStore::is_valid_hash("../etc/passwd"));
    EXPECT_FALSE(ObjectStore::is_valid_hash("0123456789abcde"));    // 15 桁
    EXPECT_FALSE(ObjectStore::is_valid_hash("0123456789abcdef0"));  // 17 桁
    EXPECT_FALSE(ObjectStore::is_valid_hash("0123456789ABCDEF"));   // 大文字
    EXPECT_FALSE(ObjectStore::is_valid_hash("0123456789abcde/"));   // 区切り
    EXPECT_FALSE(ObjectStore::is_valid_hash("0123456789ab\\cdef")); // 区切り
    EXPECT_FALSE(ObjectStore::is_valid_hash(""));
}

TEST_F(SnapshotStoreTest, RestoreStashWithMalformedHashIsNotFound)
{
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex index;
    ASSERT_TRUE(store.add_stash(index, stash_req("a.md", StashKind::Conflict, "edit"), 1).is_ok());

    // index.json 由来を装った細工 hash は object 不在として弾かれ、objects フォルダ外へ到達しない。
    auto bad = store.restore_stash("../../../../secret");
    ASSERT_TRUE(bad.is_err());
    EXPECT_EQ(bad.code(), ErrorCode::NotFound);
    EXPECT_FALSE(store.objects().contains("../../../../secret"));
}

// 細工 hash を含む index.json を読み込んでも、復元・sweep が objects 外のファイルへ届かない。
TEST_F(SnapshotStoreTest, CraftedIndexHashDoesNotEscapeObjectsDir)
{
    // objects フォルダの外（ws_dir 直下）に「消されては困る」ファイルを置く。
    SnapshotStore store(snapshots_root(), workspace_key("C:/ws"));
    SnapshotIndex seed;
    ASSERT_TRUE(store.add_stash(seed, stash_req("a.md", StashKind::Conflict, "edit"), 1).is_ok());
    ASSERT_TRUE(store.save(seed).is_ok());
    const fs::path outside = fs::path(store.ws_dir()) / "do_not_delete.txt";
    ASSERT_TRUE(pika::util::write_atomic(outside.string(), "keep me").is_ok());

    // 細工 hash を持つ退避を index に注入し、sweep を走らせる（無参照 object の物理削除経路）。
    SnapshotIndex crafted;
    IndexEntry e;
    e.rel_path = "evil.md";
    pika::core::snapshot::StashEntry s;
    s.hash = "..\\do_not_delete.txt"; // objects 外への相対参照を狙う
    s.kind = StashKind::Conflict;
    crafted.entries.push_back(e);
    // sweep は index 参照に無い実在 object を消すため、細工 hash は「参照集合」に入れず
    // 実在ファイル列挙経由でも is_valid_hash で弾かれることを確認する。
    store.sweep_unreferenced_objects(crafted);
    EXPECT_TRUE(fs::exists(outside)); // objects 外のファイルは消えない

    // 細工 hash の直接復元も object 不在として弾かれる。
    auto bad = store.restore_stash(s.hash);
    EXPECT_TRUE(bad.is_err());
}

// --- 堅牢性：解凍爆弾ガード・JSON ネスト深さ上限 -----------------------------

TEST_F(SnapshotStoreTest, DecompressRejectsOversizedFrame)
{
    using pika::core::snapshot::compress;
    using pika::core::snapshot::decompress;
    // 通常サイズは往復できる。
    const std::string ok = "round trip body\n";
    auto c = compress(ok);
    ASSERT_TRUE(c.is_ok());
    auto d = decompress(c.value());
    ASSERT_TRUE(d.is_ok());
    EXPECT_EQ(d.value(), ok);

    // 上限（40MB）を超える宣言サイズのフレームは out 確保前に Io エラーで弾く（解凍爆弾ガード）。
    const std::string big(45ull * 1024 * 1024, 'A');
    auto cb = compress(big);
    ASSERT_TRUE(cb.is_ok());
    auto db = decompress(cb.value());
    EXPECT_TRUE(db.is_err());
    EXPECT_EQ(db.code(), ErrorCode::Io);
}

TEST_F(SnapshotStoreTest, JsonParserRejectsDeeplyNestedInput)
{
    namespace json = pika::core::snapshot::json;
    // 浅いネストは通る。
    json::Value ok;
    EXPECT_TRUE(json::parse(R"({"a":[1,2,{"b":3}]})", ok));

    // 深いネスト（スタック枯渇を狙う入力）は破損扱いで false に倒す（DoS 耐性）。
    std::string deep;
    for (int i = 0; i < 5000; ++i)
    {
        deep += '[';
    }
    for (int i = 0; i < 5000; ++i)
    {
        deep += ']';
    }
    json::Value bad;
    EXPECT_FALSE(json::parse(deep, bad));
}

} // namespace
