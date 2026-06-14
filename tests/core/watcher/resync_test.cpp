// core/watcher オーバーフロー再同期の検証（sprint3 must「バッファオーバーフロー再同期」/
// should「N件同時変更で取りこぼしなし」）。
// ERROR_NOTIFY_ENUM_DIR 相当の溢れ時に監視ルートを全再列挙し mtime/サイズ→ハッシュ比較で
// 再同期して取りこぼさないことを、テンポラリフォルダの実 FS で観測する（design.md 5.2・13章 F6）。
#include "core/watcher/resync.h"

#include "core/watcher/fs_probe.h"
#include "util/atomic_file.h"
#include "util/hash.h"
#include "util/path_util.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

#include <gtest/gtest.h>

namespace
{

namespace fs = std::filesystem;

using pika::core::watcher::BaselineEntry;
using pika::core::watcher::BaselineMap;
using pika::core::watcher::FileStat;
using pika::core::watcher::FsEventKind;
using pika::core::watcher::probe;
using pika::core::watcher::resync;

class ResyncTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root_ = fs::temp_directory_path() /
                ("pika_resync_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(root_);
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    std::string abs_of(const std::string& rel) const { return (root_ / rel).generic_string(); }

    void write(const std::string& rel, const std::string& content)
    {
        const fs::path p = root_ / rel;
        fs::create_directories(p.parent_path());
        ASSERT_TRUE(pika::util::write_atomic(p.generic_string(), content).is_ok());
    }

    // 現ディスク状態から baseline エントリを作る（mtime/size は probe、hash は LF 正規化）。
    BaselineEntry baseline_entry(const std::string& rel) const
    {
        BaselineEntry e;
        FileStat st = probe(abs_of(rel));
        e.size = st.size;
        e.mtime_ns = st.mtime_ns;
        auto h = pika::core::watcher::content_hash_lf(abs_of(rel));
        e.hash_lf = h.is_ok() ? h.value() : 0;
        return e;
    }

    std::string root_str() const { return root_.generic_string(); }

    fs::path root_;
};

TEST_F(ResyncTest, DetectsAddedModifiedRemovedAfterOverflow)
{
    write("keep.md", "unchanged");
    write("edit.md", "before");
    write("gone.md", "to be deleted");

    BaselineMap base;
    base["keep.md"] = baseline_entry("keep.md");
    base["edit.md"] = baseline_entry("edit.md");
    base["gone.md"] = baseline_entry("gone.md");

    // ここで watcher バッファが溢れたと仮定し、その間に複数の変更が起きる:
    //   edit.md を変更、gone.md を削除、fresh.md を新規。
    write("edit.md", "after the change");
    fs::remove(root_ / "gone.md");
    write("fresh.md", "brand new");

    auto events = resync(root_str(), base);

    // relPath 昇順で 3 件: edit.md(Modified), fresh.md(Created), gone.md(Removed)。keep.md
    // は出ない。
    std::map<std::string, FsEventKind> got;
    for (const auto& e : events)
    {
        got[e.path] = e.kind;
    }
    ASSERT_EQ(got.count("edit.md"), 1u);
    EXPECT_EQ(got["edit.md"], FsEventKind::Modified);
    ASSERT_EQ(got.count("fresh.md"), 1u);
    EXPECT_EQ(got["fresh.md"], FsEventKind::Created);
    ASSERT_EQ(got.count("gone.md"), 1u);
    EXPECT_EQ(got["gone.md"], FsEventKind::Removed);
    EXPECT_EQ(got.count("keep.md"), 0u); // 無変化は出さない
}

TEST_F(ResyncTest, NewlineOnlyChangeIsNotModified)
{
    // CRLF/LF のみ異なる同一内容は内容変更として出さない（LF 正規化ハッシュ照合）。
    write("doc.md", "line1\nline2\n");
    BaselineMap base;
    base["doc.md"] = baseline_entry("doc.md");

    // 改行を CRLF に変える（mtime/size は変わるが LF 正規化後は同一）。
    write("doc.md", "line1\r\nline2\r\n");

    auto events = resync(root_str(), base);
    EXPECT_TRUE(events.empty());
}

TEST_F(ResyncTest, ExcludesGitAndNodeModules)
{
    write(".git/config", "[core]");
    write("node_modules/pkg/index.js", "module.exports = {}");
    write("real.md", "content");

    BaselineMap base; // 空ベースライン（全て新規候補）。
    auto events = resync(root_str(), base);

    // 除外ディレクトリ配下は再列挙対象外。real.md のみ Created。
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].path, "real.md");
    EXPECT_EQ(events[0].kind, FsEventKind::Created);
}

TEST_F(ResyncTest, NoMissAtScale)
{
    // N件(100規模)同時変更で取りこぼしが起きないこと（should）。
    BaselineMap base;
    for (int i = 0; i < 100; ++i)
    {
        const std::string rel = "f" + std::to_string(i) + ".md";
        write(rel, "v0");
        base[rel] = baseline_entry(rel);
    }
    // 全件を一斉に変更。
    for (int i = 0; i < 100; ++i)
    {
        write("f" + std::to_string(i) + ".md", "v1-changed-" + std::to_string(i));
    }
    auto events = resync(root_str(), base);
    // 100件すべてが Modified として再構成される（取りこぼしゼロ）。
    ASSERT_EQ(events.size(), 100u);
    for (const auto& e : events)
    {
        EXPECT_EQ(e.kind, FsEventKind::Modified);
    }
}

TEST_F(ResyncTest, NonAsciiFilenameDetectedAsModifiedNotRecreated)
{
    // 非ASCII（日本語）ファイル名でも relPath キーが UTF-8 で一致し、変更が Modified
    // として検知される。 rel キーが CP_ACP で化けると baseline キー(UTF-8)と不一致になり、毎回
    // Created＋Removed に 化ける（日本語専用ツールで実害大）。UTF-8 で一貫させて 1 件の Modified
    // になることを確認する。
    const std::string root_u8 = pika::util::path_to_utf8(root_);
    const std::string abs_u8 = root_u8 + "/日本語ファイル.md";
    ASSERT_TRUE(pika::util::write_atomic(abs_u8, std::string("before")).is_ok());

    BaselineMap base;
    BaselineEntry e;
    const FileStat st = probe(abs_u8);
    e.size = st.size;
    e.mtime_ns = st.mtime_ns;
    auto h = pika::core::watcher::content_hash_lf(abs_u8);
    e.hash_lf = h.is_ok() ? h.value() : 0;
    base["日本語ファイル.md"] = e;

    ASSERT_TRUE(pika::util::write_atomic(abs_u8, std::string("after the change")).is_ok());

    auto events = resync(root_u8, base);
    ASSERT_EQ(events.size(), 1u); // Created+Removed の2件に化けない
    EXPECT_EQ(events[0].path, "日本語ファイル.md");
    EXPECT_EQ(events[0].kind, FsEventKind::Modified);
}

TEST_F(ResyncTest, SubdirectoryFilesAreEnumerated)
{
    write("sub/deep/a.md", "x");
    BaselineMap base;
    auto events = resync(root_str(), base);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].path, "sub/deep/a.md");
    EXPECT_EQ(events[0].kind, FsEventKind::Created);
}

} // namespace
