// controller/dir_lister のテスト（sprint3 系統A）。
// 実 FS 列挙結果（フルパス）→ workspace::Entry 正規化が wx/FS 非依存で決定論的であり、
// 除外適用・root 越境遮断・重複畳み込み・build_tree との接続が成り立つことを観測する。
#include "controller/dir_lister.h"

#include "core/workspace/workspace_model.h"

#include <gtest/gtest.h>

namespace pc = pika::controller;
namespace cw = pika::core::workspace;

namespace
{

pc::RawListEntry file(std::string p)
{
    return pc::RawListEntry{std::move(p), false};
}
pc::RawListEntry dir(std::string p)
{
    return pc::RawListEntry{std::move(p), true};
}

} // namespace

// 絶対パスを root 起点の '/' 区切り相対パスへ正規化する（'\\' 入力も '/' 出力）。
TEST(ToRelPathTest, NormalizesBackslashToForwardSlash)
{
    EXPECT_EQ(pc::to_workspace_rel_path("C:\\work\\proj", "C:\\work\\proj\\src\\main.cpp"),
              "src/main.cpp");
}

// 大文字小文字を無視して照合する（Windows FS の非感性）。
TEST(ToRelPathTest, CaseInsensitiveRootMatch)
{
    EXPECT_EQ(pc::to_workspace_rel_path("C:/Work/Proj", "c:/work/proj/a.md"), "a.md");
}

// root 自身は相対パスを持たない（空）。
TEST(ToRelPathTest, RootItselfHasNoRelPath)
{
    EXPECT_EQ(pc::to_workspace_rel_path("C:/work/proj", "C:/work/proj"), "");
    EXPECT_EQ(pc::to_workspace_rel_path("C:/work/proj", "C:/work/proj/"), "");
}

// root 配下でないパスは空（破棄）。接頭辞が一致しても境界が違うものは配下と誤認しない。
TEST(ToRelPathTest, OutsideRootRejected)
{
    EXPECT_EQ(pc::to_workspace_rel_path("C:/work/proj", "C:/work/other/a.md"), "");
    // "C:/work/proj" が "C:/work/projection" を配下と誤認しない（セグメント境界の厳密照合）。
    EXPECT_EQ(pc::to_workspace_rel_path("C:/work/proj", "C:/work/projection/a.md"), "");
}

// 末尾区切り付きの root でも一致する（区切り正規化）。
TEST(ToRelPathTest, TrailingSeparatorOnRoot)
{
    EXPECT_EQ(pc::to_workspace_rel_path("C:/work/proj/", "C:/work/proj/sub/b.txt"), "sub/b.txt");
}

// normalize_entries: 相対化＋除外（.git/node_modules）適用＋順序保持。
TEST(NormalizeEntriesTest, RelativizesAndExcludes)
{
    const std::vector<pc::RawListEntry> raw = {
        dir("C:/ws/src"),
        file("C:/ws/src/main.cpp"),
        dir("C:/ws/.git"),
        file("C:/ws/.git/config"),
        file("C:/ws/node_modules/lib/index.js"),
        file("C:/ws/README.md"),
    };
    const auto entries = pc::normalize_entries("C:/ws", raw, {".git", "node_modules"});

    // .git 配下・node_modules 配下は除外され、残りが順序保持で相対化される。
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].rel_path, "src");
    EXPECT_TRUE(entries[0].is_dir);
    EXPECT_EQ(entries[1].rel_path, "src/main.cpp");
    EXPECT_FALSE(entries[1].is_dir);
    EXPECT_EQ(entries[2].rel_path, "README.md");
}

// root 外・root 自身は捨てる（../ 越境の遮断含む）。
TEST(NormalizeEntriesTest, DropsOutsideAndRootItself)
{
    const std::vector<pc::RawListEntry> raw = {
        file("C:/ws"),         // root 自身 → 捨てる
        file("C:/other/x.md"), // root 外 → 捨てる
        file("C:/ws/keep.md"), // 残す
    };
    const auto entries = pc::normalize_entries("C:/ws", raw, {});
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].rel_path, "keep.md");
}

// 同一バッチ内の重複 rel_path は先勝ちで 1 件に畳む（決定論）。
TEST(NormalizeEntriesTest, DeduplicatesWithinBatch)
{
    const std::vector<pc::RawListEntry> raw = {
        file("C:/ws/a.md"),
        file("C:\\ws\\a.md"), // 区切り違いでも同一相対パス
    };
    const auto entries = pc::normalize_entries("C:/ws", raw, {});
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].rel_path, "a.md");
}

// 逐次列挙の接続: normalize_entries の結果を build_tree が食え、フォルダ先行・自然順の木になる。
TEST(NormalizeEntriesTest, FeedsBuildTree)
{
    const std::vector<pc::RawListEntry> raw = {
        file("C:/ws/file10.md"),
        file("C:/ws/file2.md"),
        dir("C:/ws/sub"),
        file("C:/ws/sub/inner.txt"),
    };
    const auto entries = pc::normalize_entries("C:/ws", raw, {});
    const cw::TreeNode root = cw::build_tree(entries, {".git", "node_modules"});

    // フォルダ先行（sub が先）、ファイルは自然順（file2 が file10 より前）。
    ASSERT_EQ(root.children.size(), 3u);
    EXPECT_EQ(root.children[0].name, "sub");
    EXPECT_TRUE(root.children[0].is_dir);
    EXPECT_EQ(root.children[1].name, "file2.md");
    EXPECT_EQ(root.children[2].name, "file10.md");
}
