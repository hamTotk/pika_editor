// core/state state.json の読み書きの検証（sprint10 must「state.json の
// version・アトミック書き込み・ 未知version安全側」/ design.md 7章 K2・要件10章受け入れ基準）。
//  - 往復（window/tabs/recent/treeExpanded/modeByType/theme/lastWorkspace）
//  - 破損入力・version 欠落は Io、未知（新しい）version は読み込まず Unsupported（安全側）
//  - すべての永続化が一時ファイル→rename のアトミック書き込み（.tmp が残らない）
//  - 「最近20件」上限のクランプ
#include "core/state/state_io.h"

#include "util/atomic_file.h"

#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace
{

namespace fs = std::filesystem;

using pika::core::state::AppState;
using pika::core::state::kRecentLimit;
using pika::core::state::kStateVersion;
using pika::core::state::load_state;
using pika::core::state::parse_state;
using pika::core::state::save_state;
using pika::core::state::serialize_state;
using pika::core::state::TabState;
using pika::util::ErrorCode;

AppState sample_state()
{
    AppState s;
    s.version = kStateVersion;
    s.window.x = 100;
    s.window.y = 50;
    s.window.width = 1280;
    s.window.height = 720;
    s.window.maximized = true;
    s.last_workspace = "C:/work/project";

    TabState t1;
    t1.path = "C:/work/project/a.md";
    t1.caret = 42;
    t1.scroll = 7;
    t1.mode = "split";
    t1.preview_scroll = 13;
    s.tabs.push_back(t1);
    TabState t2;
    t2.path = "C:/work/project/b.html";
    t2.mode = "source";
    s.tabs.push_back(t2);

    s.active_tab = 1;
    s.tree_expanded = {"src", "src/core", "docs"};
    s.mode_by_type = {{".md", "split"}, {".html", "source"}};
    s.theme_current = "dark";
    s.recent.files = {"C:/work/project/a.md", "C:/work/old/b.md"};
    s.recent.folders = {"C:/work/project"};
    return s;
}

// --- 往復 --------------------------------------------------------------------

TEST(StateIoTest, SerializeParseRoundTrip)
{
    const AppState original = sample_state();
    auto parsed = parse_state(serialize_state(original));
    ASSERT_TRUE(parsed.is_ok());
    const AppState& got = parsed.value();

    EXPECT_EQ(got.version, original.version);
    EXPECT_EQ(got.window.x, 100);
    EXPECT_EQ(got.window.y, 50);
    EXPECT_EQ(got.window.width, 1280);
    EXPECT_EQ(got.window.height, 720);
    EXPECT_TRUE(got.window.maximized);
    EXPECT_EQ(got.last_workspace, "C:/work/project");

    ASSERT_EQ(got.tabs.size(), 2u);
    EXPECT_EQ(got.tabs[0].path, "C:/work/project/a.md");
    EXPECT_EQ(got.tabs[0].caret, 42);
    EXPECT_EQ(got.tabs[0].scroll, 7);
    EXPECT_EQ(got.tabs[0].mode, "split");
    EXPECT_EQ(got.tabs[0].preview_scroll, 13);
    EXPECT_EQ(got.tabs[1].mode, "source");

    EXPECT_EQ(got.active_tab, 1);
    ASSERT_EQ(got.tree_expanded.size(), 3u);
    EXPECT_EQ(got.tree_expanded[1], "src/core");

    ASSERT_EQ(got.mode_by_type.size(), 2u);
    EXPECT_EQ(got.mode_by_type[0].first, ".md");
    EXPECT_EQ(got.mode_by_type[0].second, "split");
    EXPECT_EQ(got.mode_by_type[1].first, ".html");

    EXPECT_EQ(got.theme_current, "dark");
    ASSERT_EQ(got.recent.files.size(), 2u);
    EXPECT_EQ(got.recent.files[0], "C:/work/project/a.md");
    ASSERT_EQ(got.recent.folders.size(), 1u);
}

// --- 破損・version 異常（K2 安全側） -----------------------------------------

TEST(StateIoTest, BrokenJsonIsClassifiedAsIo)
{
    auto r = parse_state("{ this is not json ");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.code(), ErrorCode::Io);
}

TEST(StateIoTest, MissingVersionIsRejected)
{
    auto r = parse_state(R"({"tabs":[]})");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.code(), ErrorCode::Io);
}

TEST(StateIoTest, UnknownNewerVersionFailsSafe)
{
    // 未知（新しい）version は読み込まず・書き戻さず・再生成もせず安全側（K2 /
    // 要件10章受け入れ基準）。
    const std::string future =
        R"({"version":)" + std::to_string(kStateVersion + 1) + R"(,"tabs":[]})";
    auto r = parse_state(future);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.code(), ErrorCode::Unsupported);
}

TEST(StateIoTest, CurrentVersionIsAccepted)
{
    const std::string cur = R"({"version":)" + std::to_string(kStateVersion) + R"(,"tabs":[]})";
    auto r = parse_state(cur);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().version, kStateVersion);
}

// --- 「最近20件」上限のクランプ ----------------------------------------------

TEST(StateIoTest, RecentClampedToLimitOnSerialize)
{
    AppState s;
    s.version = kStateVersion;
    for (std::size_t i = 0; i < kRecentLimit + 10; ++i)
    {
        s.recent.files.push_back("f" + std::to_string(i));
    }
    auto parsed = parse_state(serialize_state(s));
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_EQ(parsed.value().recent.files.size(), kRecentLimit);
    // 先頭（新しい順の前提）が残る。
    EXPECT_EQ(parsed.value().recent.files.front(), "f0");
}

// --- FS：往復・アトミック書き込み・初回ロード --------------------------------

class StateIoFsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        dir_ = fs::temp_directory_path() /
               ("pika_stateio_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(dir_);
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    std::string state_path() const { return (dir_ / "state.json").string(); }
    fs::path dir_;
};

TEST_F(StateIoFsTest, SaveLoadRoundTrip)
{
    const AppState original = sample_state();
    ASSERT_TRUE(save_state(state_path(), original).is_ok());

    auto loaded = load_state(state_path());
    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(loaded.value().last_workspace, "C:/work/project");
    ASSERT_EQ(loaded.value().tabs.size(), 2u);
    EXPECT_EQ(loaded.value().active_tab, 1);
}

TEST_F(StateIoFsTest, LoadMissingReturnsFreshState)
{
    auto loaded = load_state(state_path()); // 存在しない＝初回起動
    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(loaded.value().version, kStateVersion);
    EXPECT_TRUE(loaded.value().tabs.empty());
    EXPECT_EQ(loaded.value().active_tab, -1);
}

TEST_F(StateIoFsTest, SaveIsAtomicNoTempLeftBehind)
{
    ASSERT_TRUE(save_state(state_path(), sample_state()).is_ok());
    int tmp = 0;
    for (const auto& e : fs::directory_iterator(dir_))
    {
        if (e.path().extension() == ".tmp")
        {
            ++tmp;
        }
    }
    EXPECT_EQ(tmp, 0); // 一時ファイルは rename で消費される（アトミック書き込み）
}

TEST_F(StateIoFsTest, CorruptOnDiskIsClassifiedAsIo)
{
    ASSERT_TRUE(pika::util::write_atomic(state_path(), "{ broken ").is_ok());
    auto loaded = load_state(state_path());
    ASSERT_TRUE(loaded.is_err());
    EXPECT_EQ(loaded.code(), ErrorCode::Io);
}

TEST_F(StateIoFsTest, UnknownVersionOnDiskIsNotRewritten)
{
    // 未知 version をディスクに置き、load
    // が安全側（読み込まず・書き戻さず）に倒れることを確認する。
    const std::string future =
        R"({"version":)" + std::to_string(kStateVersion + 5) + R"(,"tabs":[]})";
    ASSERT_TRUE(pika::util::write_atomic(state_path(), future).is_ok());

    auto loaded = load_state(state_path());
    ASSERT_TRUE(loaded.is_err());
    EXPECT_EQ(loaded.code(), ErrorCode::Unsupported);

    // 呼び出し側が再生成しない限りディスクは未知 version のまま（旧版が新版状態を破壊しない）。
    auto bytes = pika::util::read_all(state_path());
    ASSERT_TRUE(bytes.is_ok());
    EXPECT_EQ(bytes.value(), future);
}

} // namespace
