// core/snapshot ObjectStore の検証（sprint5 must「内容ハッシュ名で格納・復元で一致」
// 「index破損復元 = objects 走査から退避一覧」）。重複排除・往復・走査復元を実 FS で観測する。
#include "core/snapshot/object_store.h"

#include "util/hash.h"
#include "util/path_util.h"

#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace
{

namespace fs = std::filesystem;

using pika::core::snapshot::ObjectStore;
using pika::core::snapshot::RecoveredStash;
using pika::core::snapshot::StashKind;

class ObjectStoreTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        dir_ = fs::temp_directory_path() /
               ("pika_objstore_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(dir_);
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    std::string objects_dir() const { return (dir_ / "objects").string(); }
    fs::path dir_;
};

TEST_F(ObjectStoreTest, PutGetRoundTrip)
{
    ObjectStore store(objects_dir());
    const std::string content = "ベースライン内容\nline2\n";
    auto put = store.put(content);
    ASSERT_TRUE(put.is_ok());
    // object 名は内容（原文）の XXH3-64 hex。
    EXPECT_EQ(put.value(), pika::util::xxh3_64_hex(content));

    auto got = store.get(put.value());
    ASSERT_TRUE(got.is_ok());
    EXPECT_EQ(got.value(), content);
}

TEST_F(ObjectStoreTest, DeduplicatesSameContent)
{
    ObjectStore store(objects_dir());
    const std::string content = "same content twice";
    auto a = store.put(content);
    auto b = store.put(content);
    ASSERT_TRUE(a.is_ok());
    ASSERT_TRUE(b.is_ok());
    EXPECT_EQ(a.value(), b.value());
    // 物理 object は 1 つだけ（重複排除）。
    EXPECT_EQ(store.list_objects().size(), 1u);
}

TEST_F(ObjectStoreTest, NonAsciiDirectoryRoundTrips)
{
    // 非ASCII（日本語）を含む objects ディレクトリでも put→get が往復し、実体が UTF-8 で解決した
    // 正しいパスへ作られる（fs::path(std::string) の CP_ACP 誤デコードで別パスに置かれない）。
    const std::string objdir = pika::util::path_to_utf8(dir_) + "/日本語フォルダ/objects";
    ObjectStore store(objdir);
    const std::string content = "本文\nテスト\n";
    auto put = store.put(content);
    ASSERT_TRUE(put.is_ok());

    // UTF-8 で解決した期待パスに実体がある（CP_ACP 誤配置でない）。
    const fs::path expected = pika::util::utf8_to_path(objdir) / put.value();
    EXPECT_TRUE(fs::exists(expected));

    auto got = store.get(put.value());
    ASSERT_TRUE(got.is_ok());
    EXPECT_EQ(got.value(), content);
}

TEST_F(ObjectStoreTest, GetMissingReturnsNotFound)
{
    ObjectStore store(objects_dir());
    auto got = store.get("deadbeefdeadbeef");
    ASSERT_TRUE(got.is_err());
}

TEST_F(ObjectStoreTest, RemoveDeletesObjectAndMeta)
{
    ObjectStore store(objects_dir());
    auto put = store.put_stash("stash body", "a.txt", StashKind::Conflict, 100, 1, "");
    ASSERT_TRUE(put.is_ok());
    EXPECT_TRUE(store.contains(put.value()));

    store.remove(put.value());
    EXPECT_FALSE(store.contains(put.value()));
    EXPECT_TRUE(store.scan_recoverable_stashes().empty()); // サイドカーも消えている
}

TEST_F(ObjectStoreTest, ScanRecoversStashFromSidecar)
{
    ObjectStore store(objects_dir());
    auto a = store.put_stash("incoming body", "docs/a.md", StashKind::Incoming, 1700, 3, "");
    auto b =
        store.put_stash("baseline body", "docs/b.md", StashKind::BaselineReplace, 1800, 4, "bx");
    ASSERT_TRUE(a.is_ok());
    ASSERT_TRUE(b.is_ok());

    // index.json が無くても、objects のサイドカー走査だけで退避一覧を再構築できる（D1）。
    auto recovered = store.scan_recoverable_stashes();
    ASSERT_EQ(recovered.size(), 2u);

    bool saw_incoming = false;
    bool saw_baseline = false;
    for (const RecoveredStash& r : recovered)
    {
        if (r.kind == StashKind::Incoming)
        {
            saw_incoming = true;
            EXPECT_EQ(r.rel_path, "docs/a.md");
            EXPECT_EQ(r.time, 1700);
            EXPECT_EQ(r.index_gen, 3);
            // 退避 object から実内容を復元できる。
            auto body = store.get(r.object_hash);
            ASSERT_TRUE(body.is_ok());
            EXPECT_EQ(body.value(), "incoming body");
        }
        else if (r.kind == StashKind::BaselineReplace)
        {
            saw_baseline = true;
            EXPECT_EQ(r.rel_path, "docs/b.md");
            EXPECT_EQ(r.batch_id, "bx");
        }
    }
    EXPECT_TRUE(saw_incoming);
    EXPECT_TRUE(saw_baseline);
}

TEST_F(ObjectStoreTest, SharedContentKeepsSeparateSidecarsForEachStash)
{
    ObjectStore store(objects_dir());
    // 内容一致の2退避（別ファイル・別種別）。object は重複排除で1つだが、復元メタは2件残るべき。
    // 旧実装はサイドカーを object hash 名で上書きしたため、2件目が1件目のメタを後勝ちで潰し、
    // index.json 破損時の復元一覧から1件分（どのファイルの退避か）が消えていた（D1 取りこぼし）。
    auto a = store.put_stash("identical body", "a.md", StashKind::Conflict, 100, 1, "");
    auto b = store.put_stash("identical body", "b.md", StashKind::Incoming, 200, 2, "");
    ASSERT_TRUE(a.is_ok());
    ASSERT_TRUE(b.is_ok());
    EXPECT_EQ(a.value(), b.value());            // 同一 object（重複排除）
    EXPECT_EQ(store.list_objects().size(), 1u); // 物理 object は 1 つ

    auto recovered = store.scan_recoverable_stashes();
    ASSERT_EQ(recovered.size(), 2u); // 退避単位のサイドカーで両方の復元メタが残る
    bool saw_a = false;
    bool saw_b = false;
    for (const RecoveredStash& r : recovered)
    {
        if (r.rel_path == "a.md" && r.kind == StashKind::Conflict)
        {
            saw_a = true;
        }
        if (r.rel_path == "b.md" && r.kind == StashKind::Incoming)
        {
            saw_b = true;
        }
        EXPECT_EQ(r.object_hash, a.value()); // 両者とも同一 object を指す
    }
    EXPECT_TRUE(saw_a);
    EXPECT_TRUE(saw_b);
}

TEST_F(ObjectStoreTest, ScanSkipsSidecarWithMissingObject)
{
    ObjectStore store(objects_dir());
    auto put = store.put_stash("body", "x.txt", StashKind::Rollback, 1, 1, "");
    ASSERT_TRUE(put.is_ok());

    // object 本体だけを消す（サイドカーは残る）。復元不能なメタは一覧から除外される。
    fs::remove(fs::path(objects_dir()) / put.value());
    EXPECT_TRUE(store.scan_recoverable_stashes().empty());
}

} // namespace
