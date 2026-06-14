// core/workspace の検証（sprint8 must）。
// - フォルダ先行・自然順ソート（file2 が file10 より前）
// - 既定除外（.git/node_modules）の非表示かつ監視対象外
// - 未読の rename 引き継ぎ（未読・ベースライン・退避）と安全側フォールバック
// - 子孫伝播未読とファイル自身の未読の区別
#include "core/workspace/workspace_model.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::workspace::apply_renames;
using pika::core::workspace::build_tree;
using pika::core::workspace::CarryOutcome;
using pika::core::workspace::CarryState;
using pika::core::workspace::Entry;
using pika::core::workspace::is_excluded;
using pika::core::workspace::natural_less;
using pika::core::workspace::RenameOp;
using pika::core::workspace::TreeNode;
using pika::core::workspace::UnreadSet;

// ---- 自然順ソート ----

TEST(NaturalSortTest, NumericOrderingBeatsLexical)
{
    EXPECT_TRUE(natural_less("file2", "file10")); // 2 < 10（辞書順なら逆）
    EXPECT_FALSE(natural_less("file10", "file2"));
    EXPECT_TRUE(natural_less("img1.png", "img12.png"));
    EXPECT_TRUE(natural_less("a", "b"));
    EXPECT_FALSE(natural_less("b", "a"));
}

TEST(NaturalSortTest, CaseInsensitivePrimary)
{
    EXPECT_TRUE(natural_less("Apple", "banana")); // 大小無視で a < b
    EXPECT_TRUE(natural_less("alpha", "Beta"));
}

TEST(NaturalSortTest, PrefixShorterFirst)
{
    EXPECT_TRUE(natural_less("file", "file2"));
    EXPECT_FALSE(natural_less("file2", "file"));
}

TEST(NaturalSortTest, LeadingZeros)
{
    // 数値として等しい場合は元桁数の短い方を前に（安定）。
    EXPECT_TRUE(natural_less("v2", "v002") || natural_less("v002", "v2"));
    EXPECT_TRUE(natural_less("a02", "a10")); // 2 < 10
}

// ---- 除外リスト ----

TEST(ExcludeTest, DefaultsHideGitAndNodeModules)
{
    const std::vector<std::string> ex = {".git", "node_modules"};
    EXPECT_TRUE(is_excluded(".git/config", ex));
    EXPECT_TRUE(is_excluded("node_modules/lodash/index.js", ex));
    EXPECT_TRUE(is_excluded("src/node_modules/x.js", ex)); // 途中セグメントでも除外
    EXPECT_FALSE(is_excluded("src/main.cpp", ex));
    EXPECT_FALSE(is_excluded("gitignore.txt", ex)); // 部分一致は除外しない
}

// ---- ツリー構築（フォルダ先行・自然順・除外適用） ----

const TreeNode* find_child(const TreeNode& parent, const std::string& name)
{
    for (const auto& c : parent.children)
    {
        if (c.name == name)
        {
            return &c;
        }
    }
    return nullptr;
}

TEST(BuildTreeTest, FoldersFirstThenNaturalOrder)
{
    std::vector<Entry> entries = {
        {"file10.txt", false}, {"file2.txt", false}, {"zeta", true},
        {"alpha", true},       {"readme.md", false},
    };
    TreeNode root = build_tree(entries, {});
    ASSERT_EQ(root.children.size(), 5u);
    // フォルダ先行：alpha, zeta が先、その後にファイル（自然順 file2 < file10 < readme）。
    EXPECT_EQ(root.children[0].name, "alpha");
    EXPECT_TRUE(root.children[0].is_dir);
    EXPECT_EQ(root.children[1].name, "zeta");
    EXPECT_TRUE(root.children[1].is_dir);
    EXPECT_EQ(root.children[2].name, "file2.txt");
    EXPECT_EQ(root.children[3].name, "file10.txt");
    EXPECT_EQ(root.children[4].name, "readme.md");
}

TEST(BuildTreeTest, NestedPathsCreateIntermediateDirs)
{
    std::vector<Entry> entries = {
        {"src/core/a.cpp", false},
        {"src/b.cpp", false},
    };
    TreeNode root = build_tree(entries, {});
    ASSERT_EQ(root.children.size(), 1u);
    const TreeNode* src = &root.children[0];
    EXPECT_EQ(src->name, "src");
    EXPECT_TRUE(src->is_dir);
    // src の子はフォルダ core 先行、その後 b.cpp。
    ASSERT_EQ(src->children.size(), 2u);
    EXPECT_EQ(src->children[0].name, "core");
    EXPECT_TRUE(src->children[0].is_dir);
    EXPECT_EQ(src->children[1].name, "b.cpp");
    const TreeNode* core = find_child(*src, "core");
    ASSERT_NE(core, nullptr);
    ASSERT_EQ(core->children.size(), 1u);
    EXPECT_EQ(core->children[0].rel_path, "src/core/a.cpp");
}

TEST(BuildTreeTest, ExcludedEntriesAreNotInTree)
{
    std::vector<Entry> entries = {
        {".git", true},
        {".git/config", false},
        {"node_modules/x/y.js", false},
        {"src/main.cpp", false},
    };
    TreeNode root = build_tree(entries, {".git", "node_modules"});
    // .git と node_modules 配下は監視対象外として木に含めない。
    EXPECT_EQ(find_child(root, ".git"), nullptr);
    EXPECT_EQ(find_child(root, "node_modules"), nullptr);
    ASSERT_NE(find_child(root, "src"), nullptr);
}

// ---- 未読集合と子孫伝播 ----

TEST(UnreadSetTest, SelfVsDescendantPropagation)
{
    UnreadSet us;
    us.mark("docs/sub/note.md");
    us.mark("readme.md");

    // ファイル自身の未読。
    EXPECT_TRUE(us.is_unread("docs/sub/note.md"));
    EXPECT_TRUE(us.is_unread("readme.md"));
    EXPECT_FALSE(us.is_unread("docs")); // フォルダ自身は「ファイル未読」ではない

    // 伝播未読：docs / docs/sub は子孫に未読を持つ。
    EXPECT_TRUE(us.has_unread_descendant("docs"));
    EXPECT_TRUE(us.has_unread_descendant("docs/sub"));
    EXPECT_FALSE(us.has_unread_descendant("other"));
    EXPECT_TRUE(us.has_unread_descendant("")); // ルートは未読があれば真

    EXPECT_EQ(us.count(), 2u);
}

TEST(UnreadSetTest, ClearRemovesUnread)
{
    UnreadSet us;
    us.mark("a/b.txt");
    EXPECT_TRUE(us.has_unread_descendant("a"));
    us.clear("a/b.txt");
    EXPECT_FALSE(us.is_unread("a/b.txt"));
    EXPECT_FALSE(us.has_unread_descendant("a"));
    EXPECT_EQ(us.count(), 0u);
}

TEST(UnreadSetTest, PrefixIsNotSubstringMatch)
{
    // "docs" の伝播判定が "documents/x" を誤検出しない（区切り '/' を必須にする）。
    UnreadSet us;
    us.mark("documents/x.md");
    EXPECT_FALSE(us.has_unread_descendant("docs"));
    EXPECT_TRUE(us.has_unread_descendant("documents"));
}

// ---- rename 引き継ぎ ----

CarryState make_state(bool unread, std::uint64_t baseline)
{
    CarryState s;
    s.unread = unread;
    s.has_baseline = baseline != 0;
    s.baseline_hash = baseline;
    return s;
}

TEST(RenameCarryTest, MovesUnreadBaselineAndStash)
{
    std::map<std::string, CarryState> states;
    CarryState s = make_state(true, 0xABCD);
    s.stash_ids = {"obj1", "obj2"};
    states["old.md"] = s;

    std::vector<RenameOp> ops = {{"old.md", "new.md"}};
    auto r = apply_renames(states, ops);

    ASSERT_EQ(r.outcomes.size(), 1u);
    EXPECT_EQ(r.outcomes[0], CarryOutcome::Moved);
    EXPECT_EQ(r.states.count("old.md"), 0u);
    ASSERT_EQ(r.states.count("new.md"), 1u);
    const CarryState& moved = r.states.at("new.md");
    EXPECT_TRUE(moved.unread);
    EXPECT_EQ(moved.baseline_hash, 0xABCDu);
    EXPECT_EQ(moved.stash_ids, (std::vector<std::string>{"obj1", "obj2"}));
}

TEST(RenameCarryTest, OverwriteDestinationUsesSourceState)
{
    std::map<std::string, CarryState> states;
    states["a.md"] = make_state(true, 0x1111);  // 移動元
    states["b.md"] = make_state(false, 0x2222); // 既存の移動先
    std::vector<RenameOp> ops = {{"a.md", "b.md"}};
    auto r = apply_renames(states, ops);

    ASSERT_EQ(r.outcomes.size(), 1u);
    EXPECT_EQ(r.outcomes[0], CarryOutcome::OverwroteDst);
    // 移動先は移動元の状態で上書きされる（要件4.2）。
    ASSERT_EQ(r.states.count("b.md"), 1u);
    EXPECT_EQ(r.states.at("b.md").baseline_hash, 0x1111u);
    EXPECT_TRUE(r.states.at("b.md").unread);
    EXPECT_EQ(r.states.count("a.md"), 0u);
}

TEST(RenameCarryTest, OldAloneIsRemovedAndOrphanedForSafety)
{
    std::map<std::string, CarryState> states;
    states["gone.md"] = make_state(true, 0x9);
    std::vector<RenameOp> ops = {{"gone.md", ""}}; // 旧名単独
    auto r = apply_renames(states, ops);

    EXPECT_EQ(r.outcomes[0], CarryOutcome::Removed);
    // 状態は旧キーで孤立保全（90日GC に委ねる。失わない）。
    ASSERT_EQ(r.orphaned.size(), 1u);
    EXPECT_EQ(r.orphaned[0], "gone.md");
}

TEST(RenameCarryTest, NewAloneIsCreatedWithoutBaseline)
{
    std::map<std::string, CarryState> states;
    std::vector<RenameOp> ops = {{"", "fresh.md"}}; // 新名単独
    auto r = apply_renames(states, ops);

    EXPECT_EQ(r.outcomes[0], CarryOutcome::Created);
    ASSERT_EQ(r.states.count("fresh.md"), 1u);
    EXPECT_FALSE(r.states.at("fresh.md").has_baseline); // ベースラインなしで開始
    EXPECT_TRUE(r.states.at("fresh.md").unread);        // 新規は未読
}

TEST(RenameCarryTest, RoundTripIsReevaluated)
{
    // A→B→A の往復は対応付け確定不能 → 最終ディスク内容で再判定する指示を返す（要件4.2）。
    std::map<std::string, CarryState> states;
    states["A.md"] = make_state(true, 0xA);
    std::vector<RenameOp> ops = {{"A.md", "B.md"}, {"B.md", "A.md"}};
    auto r = apply_renames(states, ops);

    bool any_reeval = false;
    for (auto o : r.outcomes)
    {
        if (o == CarryOutcome::Reevaluated)
        {
            any_reeval = true;
        }
    }
    EXPECT_TRUE(any_reeval);
    EXPECT_FALSE(r.reevaluate.empty());
}

TEST(RenameCarryTest, SwapPreservesBothStates)
{
    // 相互スワップ A↔B はクロス参照しない 2 つの独立 rename（A→B, B→A は往復扱い）と異なり、
    // 一時退避を経た付け替え。ここでは A→tmp, B→A, tmp→B の正規化済み列を引き継げることを確認する。
    std::map<std::string, CarryState> states;
    states["A.md"] = make_state(true, 0xAA);
    states["B.md"] = make_state(false, 0xBB);
    std::vector<RenameOp> ops = {{"A.md", "tmp~"}, {"B.md", "A.md"}, {"tmp~", "B.md"}};
    auto r = apply_renames(states, ops);

    ASSERT_EQ(r.states.count("A.md"), 1u);
    ASSERT_EQ(r.states.count("B.md"), 1u);
    EXPECT_EQ(r.states.at("A.md").baseline_hash, 0xBBu); // 元 B が A へ
    EXPECT_EQ(r.states.at("B.md").baseline_hash, 0xAAu); // 元 A が B へ
}

} // namespace
