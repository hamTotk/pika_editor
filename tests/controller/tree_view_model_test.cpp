// controller/tree_view_model の検証（sprint1 must/should）。
// - 種別アイコン分類: 代表拡張子 → カテゴリ、未知拡張子 → Unknown フォールバック（ui-design 6章）
// - 状態マーク: build_tree()+UnreadSet からファイル ±/◆、フォルダ伝播 ±淡を決定論付与（ui-design
// 5章）
// - 重畳優先: 削除済み ＞ 未保存 ＞ 差分あり（要件5.3）
// - フォルダ伝播 ±（淡）と ファイル自身の ±（実心）を別値で区別（criteria）
// - 純粋関数（同一入力で同一出力）
// - メッセージ写像（列挙値 → 記号/日本語ラベルの単一定義。should）
#include "controller/tree_view_messages.h"
#include "controller/tree_view_model.h"

#include <gtest/gtest.h>

#include <functional>

namespace
{

using pika::controller::build_tree_view_model;
using pika::controller::classify_icon;
using pika::controller::icon_category_label;
using pika::controller::IconCategory;
using pika::controller::NodeStateInput;
using pika::controller::resolve_file_mark;
using pika::controller::state_mark_label;
using pika::controller::state_mark_symbol;
using pika::controller::StateMark;
using pika::controller::TreeRowVm;
using pika::core::workspace::build_tree;
using pika::core::workspace::Entry;
using pika::core::workspace::TreeNode;
using pika::core::workspace::UnreadSet;

// ---- 種別アイコン分類（ui-design 6章） ----

TEST(ClassifyIconTest, RepresentativeExtensionsMapToCategory)
{
    // コード/マークアップ
    EXPECT_EQ(classify_icon("app.ts"), IconCategory::Code);
    EXPECT_EQ(classify_icon("main.js"), IconCategory::Code);
    EXPECT_EQ(classify_icon("index.html"), IconCategory::Code);
    EXPECT_EQ(classify_icon("page.htm"), IconCategory::Code);
    EXPECT_EQ(classify_icon("data.xml"), IconCategory::Code);
    // データ
    EXPECT_EQ(classify_icon("config.json"), IconCategory::Data);
    // 設定
    EXPECT_EQ(classify_icon("ci.yaml"), IconCategory::Config);
    EXPECT_EQ(classify_icon("ci.yml"), IconCategory::Config);
    EXPECT_EQ(classify_icon("vcpkg.toml"), IconCategory::Config);
    EXPECT_EQ(classify_icon("setup.ini"), IconCategory::Config);
    // スクリプト
    EXPECT_EQ(classify_icon("build.sh"), IconCategory::Script);
    EXPECT_EQ(classify_icon("run.ps1"), IconCategory::Script);
    EXPECT_EQ(classify_icon("go.bat"), IconCategory::Script);
    // 画像
    EXPECT_EQ(classify_icon("logo.png"), IconCategory::Image);
    EXPECT_EQ(classify_icon("photo.jpg"), IconCategory::Image);
    EXPECT_EQ(classify_icon("anim.gif"), IconCategory::Image);
    EXPECT_EQ(classify_icon("pic.webp"), IconCategory::Image);
    EXPECT_EQ(classify_icon("art.bmp"), IconCategory::Image);
    EXPECT_EQ(classify_icon("fav.ico"), IconCategory::Image);
    EXPECT_EQ(classify_icon("icon.svg"), IconCategory::Image);
    // テキスト/文書
    EXPECT_EQ(classify_icon("readme.md"), IconCategory::Text);
    EXPECT_EQ(classify_icon("notes.markdown"), IconCategory::Text);
    EXPECT_EQ(classify_icon("memo.txt"), IconCategory::Text);
    EXPECT_EQ(classify_icon("rows.csv"), IconCategory::Text);
    EXPECT_EQ(classify_icon("trace.log"), IconCategory::Text);
}

TEST(ClassifyIconTest, UnknownExtensionFallsBackToUnknown)
{
    EXPECT_EQ(classify_icon("a.cpp"), IconCategory::Unknown); // 表に無い拡張子
    EXPECT_EQ(classify_icon("binary.exe"), IconCategory::Unknown);
    EXPECT_EQ(classify_icon("noext"), IconCategory::Unknown);      // 拡張子なし
    EXPECT_EQ(classify_icon(".gitignore"), IconCategory::Unknown); // 先頭ドットのみ
    EXPECT_EQ(classify_icon("trailing."), IconCategory::Unknown);  // 末尾ドット（空拡張子）
}

TEST(ClassifyIconTest, ExtensionIsCaseInsensitive)
{
    EXPECT_EQ(classify_icon("README.MD"), IconCategory::Text);
    EXPECT_EQ(classify_icon("Photo.PNG"), IconCategory::Image);
    EXPECT_EQ(classify_icon("App.TS"), IconCategory::Code);
}

TEST(ClassifyIconTest, AcceptsBareExtension)
{
    // 拡張子だけを渡しても写像できる（呼び出し側の利便）。
    EXPECT_EQ(classify_icon("json"), IconCategory::Data);
    EXPECT_EQ(classify_icon("toml"), IconCategory::Config);
}

// ---- 状態マークの重畳優先（要件5.3：削除済み ＞ 未保存 ＞ 差分あり） ----

TEST(ResolveFileMarkTest, PriorityDeletedBeatsAll)
{
    NodeStateInput s;
    s.deleted = true;
    s.unsaved = true;
    s.unread = true;
    EXPECT_EQ(resolve_file_mark(s), StateMark::Deleted);
}

TEST(ResolveFileMarkTest, PriorityUnsavedBeatsDiff)
{
    NodeStateInput s;
    s.unsaved = true;
    s.unread = true;
    EXPECT_EQ(resolve_file_mark(s), StateMark::Unsaved);
}

TEST(ResolveFileMarkTest, UnreadWithBaselineIsDiff)
{
    NodeStateInput s;
    s.unread = true;
    s.has_baseline = true;
    EXPECT_EQ(resolve_file_mark(s), StateMark::Diff);
}

TEST(ResolveFileMarkTest, UnreadWithoutBaselineIsNew)
{
    NodeStateInput s;
    s.unread = true;
    s.has_baseline = false;
    EXPECT_EQ(resolve_file_mark(s), StateMark::New);
}

TEST(ResolveFileMarkTest, NoSignalIsNone)
{
    NodeStateInput s; // すべて既定（has_baseline=true, それ以外 false）
    EXPECT_EQ(resolve_file_mark(s), StateMark::None);
}

// ---- ツリー → ViewModel（決定論・伝播） ----

const TreeRowVm* find_child(const TreeRowVm& parent, const std::string& name)
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

TEST(BuildTreeViewModelTest, FileSelfDiffVsFolderPropagatedAreDistinct)
{
    // src/a.md（未読）, src/b.txt（既読）, docs/c.md（既読）の木。
    std::vector<Entry> entries = {
        {"src", true},  {"src/a.md", false},  {"src/b.txt", false},
        {"docs", true}, {"docs/c.md", false},
    };
    TreeNode root = build_tree(entries, {});
    UnreadSet unread;
    unread.mark("src/a.md"); // src 配下に未読あり / docs 配下にはなし

    TreeRowVm vm = build_tree_view_model(root, unread);

    const TreeRowVm* src = find_child(vm, "src");
    const TreeRowVm* docs = find_child(vm, "docs");
    ASSERT_NE(src, nullptr);
    ASSERT_NE(docs, nullptr);

    // フォルダ src は伝播 ±（淡）= DiffPropagated。docs は子孫に未読なし = None。
    EXPECT_EQ(src->mark, StateMark::DiffPropagated);
    EXPECT_EQ(docs->mark, StateMark::None);

    // ファイル src/a.md 自身は ±（実心）= Diff。両者が別値で区別できる（criteria）。
    const TreeRowVm* a = find_child(*src, "a.md");
    const TreeRowVm* b = find_child(*src, "b.txt");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->mark, StateMark::Diff);
    EXPECT_NE(a->mark, src->mark); // ファイルの ± と フォルダの伝播 ± は別値
    EXPECT_EQ(b->mark, StateMark::None);
}

TEST(BuildTreeViewModelTest, NewFileGetsDiamondMark)
{
    std::vector<Entry> entries = {{"new.md", false}, {"old.md", false}};
    TreeNode root = build_tree(entries, {});
    UnreadSet unread;
    unread.mark("new.md");
    unread.mark("old.md");

    // new.md はベースラインなし（新規）、old.md はベースラインあり（差分）。
    TreeRowVm vm = build_tree_view_model(root, unread, {"new.md"});

    const TreeRowVm* nf = find_child(vm, "new.md");
    const TreeRowVm* of = find_child(vm, "old.md");
    ASSERT_NE(nf, nullptr);
    ASSERT_NE(of, nullptr);
    EXPECT_EQ(nf->mark, StateMark::New);  // ◆
    EXPECT_EQ(of->mark, StateMark::Diff); // ±
}

TEST(BuildTreeViewModelTest, NestedPropagationReachesAncestors)
{
    // a/b/c/deep.md が未読 → a, a/b, a/b/c すべて伝播する。
    std::vector<Entry> entries = {{"a/b/c/deep.md", false}, {"a/sibling.txt", false}};
    TreeNode root = build_tree(entries, {});
    UnreadSet unread;
    unread.mark("a/b/c/deep.md");

    TreeRowVm vm = build_tree_view_model(root, unread);
    const TreeRowVm* a = find_child(vm, "a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->mark, StateMark::DiffPropagated);
    const TreeRowVm* b = find_child(*a, "b");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->mark, StateMark::DiffPropagated);
    const TreeRowVm* c = find_child(*b, "c");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->mark, StateMark::DiffPropagated);
    const TreeRowVm* deep = find_child(*c, "deep.md");
    ASSERT_NE(deep, nullptr);
    EXPECT_EQ(deep->mark, StateMark::Diff); // 葉ファイル自身は実心 ±

    // 未読を含まない兄弟ファイルはマークなし。
    const TreeRowVm* sib = find_child(*a, "sibling.txt");
    ASSERT_NE(sib, nullptr);
    EXPECT_EQ(sib->mark, StateMark::None);
}

TEST(BuildTreeViewModelTest, IconClassificationPropagatedToNodes)
{
    std::vector<Entry> entries = {
        {"dir", true}, {"dir/a.json", false}, {"dir/b.unknownext", false}};
    TreeNode root = build_tree(entries, {});
    UnreadSet unread;
    TreeRowVm vm = build_tree_view_model(root, unread);

    const TreeRowVm* dir = find_child(vm, "dir");
    ASSERT_NE(dir, nullptr);
    EXPECT_EQ(dir->icon, IconCategory::Folder);
    EXPECT_TRUE(dir->is_dir);

    const TreeRowVm* a = find_child(*dir, "a.json");
    const TreeRowVm* b = find_child(*dir, "b.unknownext");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->icon, IconCategory::Data);
    EXPECT_EQ(b->icon, IconCategory::Unknown);
    EXPECT_FALSE(a->is_dir);
}

TEST(BuildTreeViewModelTest, PureFunctionSameInputSameOutput)
{
    std::vector<Entry> entries = {
        {"src", true}, {"src/x.md", false}, {"src/y.json", false}, {"z.txt", false}};
    TreeNode root = build_tree(entries, {});
    UnreadSet unread;
    unread.mark("src/x.md");

    TreeRowVm a = build_tree_view_model(root, unread, {"src/x.md"});
    TreeRowVm b = build_tree_view_model(root, unread, {"src/x.md"});

    // 構造的に同一であることを再帰比較で観測する（決定論）。
    std::function<void(const TreeRowVm&, const TreeRowVm&)> eq = [&](const TreeRowVm& l,
                                                                     const TreeRowVm& r) {
        EXPECT_EQ(l.name, r.name);
        EXPECT_EQ(l.rel_path, r.rel_path);
        EXPECT_EQ(l.is_dir, r.is_dir);
        EXPECT_EQ(l.mark, r.mark);
        EXPECT_EQ(l.icon, r.icon);
        ASSERT_EQ(l.children.size(), r.children.size());
        for (std::size_t i = 0; i < l.children.size(); ++i)
        {
            eq(l.children[i], r.children[i]);
        }
    };
    eq(a, b);
}

TEST(BuildTreeViewModelTest, PreservesFolderFirstNaturalOrder)
{
    // build_tree のフォルダ先行・自然順整列が ViewModel に保たれる（再実装しない＝足さない）。
    std::vector<Entry> entries = {
        {"file10.txt", false}, {"file2.txt", false}, {"zdir", true}, {"adir", true}};
    TreeNode root = build_tree(entries, {});
    UnreadSet unread;
    TreeRowVm vm = build_tree_view_model(root, unread);

    ASSERT_EQ(vm.children.size(), 4u);
    // フォルダ先行（自然順 adir, zdir）→ ファイル（自然順 file2, file10）。
    EXPECT_EQ(vm.children[0].name, "adir");
    EXPECT_EQ(vm.children[1].name, "zdir");
    EXPECT_EQ(vm.children[2].name, "file2.txt");
    EXPECT_EQ(vm.children[3].name, "file10.txt");
}

// ---- メッセージ写像（should：列挙値 → 記号/日本語ラベルの単一定義） ----

TEST(TreeViewMessagesTest, StateMarkSymbols)
{
    EXPECT_EQ(state_mark_symbol(StateMark::Diff), "±");
    EXPECT_EQ(state_mark_symbol(StateMark::New), "◆");
    EXPECT_EQ(state_mark_symbol(StateMark::Unsaved), "●");
    EXPECT_EQ(state_mark_symbol(StateMark::DiffPropagated), "±");
    // 削除済みは取り消し線で表現し記号を持たない（ui-design 5章）。
    EXPECT_EQ(state_mark_symbol(StateMark::Deleted), "");
    EXPECT_EQ(state_mark_symbol(StateMark::None), "");
}

TEST(TreeViewMessagesTest, StateMarkLabelsAreJapanese)
{
    EXPECT_EQ(state_mark_label(StateMark::Diff), "差分あり");
    EXPECT_EQ(state_mark_label(StateMark::New), "新規");
    EXPECT_EQ(state_mark_label(StateMark::Deleted), "削除済み");
    EXPECT_EQ(state_mark_label(StateMark::Unsaved), "未保存");
    EXPECT_EQ(state_mark_label(StateMark::DiffPropagated), "配下に差分あり");
}

TEST(TreeViewMessagesTest, IconCategoryLabelsAreJapanese)
{
    EXPECT_EQ(icon_category_label(IconCategory::Folder), "フォルダ");
    EXPECT_EQ(icon_category_label(IconCategory::Code), "コード");
    EXPECT_EQ(icon_category_label(IconCategory::Unknown), "ファイル");
}

} // namespace
