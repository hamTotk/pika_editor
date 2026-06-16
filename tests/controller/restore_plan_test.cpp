// controller/restore_plan の検証（sprint7 must#1）。
// - AppState → UI 復元素材（RestorePlan）の決定論再構成。
// - 欠落フィールドの安全な既定値補完（diff_on=false・tree_pane_collapsed=false）。
// - 未知 version は読まない/復元しない（restorable=false。design 7章 K2）。
// - 表示モード文字列の正規化（"diff"→source+diff_on / 不明→source。ui-design 14章）。
// - テーマ・recent クランプ・アクティブタブクランプ・ウィンドウジオメトリ・決定論。
#include "controller/restore_plan.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using pika::controller::build_restore_plan;
using pika::controller::normalize_mode;
using pika::controller::normalize_theme;
using pika::controller::RestorePlan;
using pika::controller::ThemeKind;
using pika::controller::ViewMode;
using pika::core::state::AppState;
using pika::core::state::kStateVersion;
using pika::core::state::TabState;

// ---- version 安全側（K2） ----

TEST(RestorePlanTest, KnownVersionIsRestorable)
{
    AppState s;
    s.version = kStateVersion;
    const RestorePlan p = build_restore_plan(s);
    EXPECT_TRUE(p.restorable);
}

TEST(RestorePlanTest, UnknownNewerVersionIsNotRestored)
{
    // 未知（新しい）version は復元しない＝既定起動（旧版が新版状態を破壊しない）。
    AppState s;
    s.version = kStateVersion + 1;
    s.tabs.push_back(TabState{"C:/ws/a.md", 0, 0, "preview", 0});
    const RestorePlan p = build_restore_plan(s);
    EXPECT_FALSE(p.restorable);
    EXPECT_TRUE(p.tabs.empty()); // 中身は読まない
}

// ---- 表示モードの正規化（3モード＋差分トグル。ui-design 14章） ----

TEST(RestorePlanTest, NormalizeModeMapsLegacyDiffToSourcePlusDiffOn)
{
    ViewMode mode = ViewMode::Preview;
    bool diff_on = false;
    normalize_mode("diff", mode, diff_on);
    EXPECT_EQ(mode, ViewMode::Source);
    EXPECT_TRUE(diff_on);
}

TEST(RestorePlanTest, NormalizeModeMapsKnownModes)
{
    ViewMode mode = ViewMode::Source;
    bool diff_on = true;
    normalize_mode("split", mode, diff_on);
    EXPECT_EQ(mode, ViewMode::Split);
    EXPECT_FALSE(diff_on);

    normalize_mode("preview", mode, diff_on);
    EXPECT_EQ(mode, ViewMode::Preview);
    EXPECT_FALSE(diff_on);

    normalize_mode("source", mode, diff_on);
    EXPECT_EQ(mode, ViewMode::Source);
    EXPECT_FALSE(diff_on);
}

TEST(RestorePlanTest, NormalizeModeUnknownFallsBackToSourceNoDiff)
{
    // 不明・空文字は安全側の (Source, false)。
    ViewMode mode = ViewMode::Preview;
    bool diff_on = true;
    normalize_mode("garbage", mode, diff_on);
    EXPECT_EQ(mode, ViewMode::Source);
    EXPECT_FALSE(diff_on);

    normalize_mode("", mode, diff_on);
    EXPECT_EQ(mode, ViewMode::Source);
    EXPECT_FALSE(diff_on);
}

// ---- 欠落フィールドの安全な既定値補完 ----

TEST(RestorePlanTest, MissingDiffOnAndTreePaneDefaultToFalse)
{
    // mode=source（差分なし）のタブで diff_on=false が補完され、
    // tree_pane_collapsed も既定 false になる。
    AppState s;
    s.version = kStateVersion;
    s.tabs.push_back(TabState{"C:/ws/a.md", 0, 0, "source", 0});
    const RestorePlan p = build_restore_plan(s);
    ASSERT_EQ(p.tabs.size(), 1u);
    EXPECT_FALSE(p.tabs[0].diff_on);
    EXPECT_FALSE(p.tree_pane_collapsed);
}

TEST(RestorePlanTest, EmptyStateProducesEmptyButRestorablePlan)
{
    AppState s;
    s.version = kStateVersion;
    const RestorePlan p = build_restore_plan(s);
    EXPECT_TRUE(p.restorable);
    EXPECT_TRUE(p.tabs.empty());
    EXPECT_EQ(p.active_tab, -1);
    EXPECT_FALSE(p.tree_pane_collapsed);
    EXPECT_EQ(p.theme, ThemeKind::System);
    EXPECT_FALSE(p.has_window_geometry);
}

// ---- アクティブタブのクランプ ----

TEST(RestorePlanTest, ActiveTabClampedToRange)
{
    AppState s;
    s.version = kStateVersion;
    s.tabs.push_back(TabState{"C:/ws/a.md", 0, 0, "source", 0});
    s.tabs.push_back(TabState{"C:/ws/b.md", 0, 0, "source", 0});
    s.active_tab = 5; // 範囲外
    const RestorePlan p = build_restore_plan(s);
    EXPECT_EQ(p.active_tab, -1);
}

TEST(RestorePlanTest, ActiveTabPreservedWhenInRange)
{
    AppState s;
    s.version = kStateVersion;
    s.tabs.push_back(TabState{"C:/ws/a.md", 0, 0, "source", 0});
    s.tabs.push_back(TabState{"C:/ws/b.md", 0, 0, "source", 0});
    s.active_tab = 1;
    const RestorePlan p = build_restore_plan(s);
    EXPECT_EQ(p.active_tab, 1);
}

TEST(RestorePlanTest, NegativeActiveTabBecomesNoActive)
{
    AppState s;
    s.version = kStateVersion;
    s.active_tab = -1;
    const RestorePlan p = build_restore_plan(s);
    EXPECT_EQ(p.active_tab, -1);
}

// ---- ウィンドウジオメトリ ----

TEST(RestorePlanTest, ZeroSizeWindowHasNoGeometry)
{
    AppState s;
    s.version = kStateVersion;
    s.window.width = 0;
    s.window.height = 0;
    const RestorePlan p = build_restore_plan(s);
    EXPECT_FALSE(p.has_window_geometry);
}

TEST(RestorePlanTest, PositiveSizeWindowHasGeometry)
{
    AppState s;
    s.version = kStateVersion;
    s.window.x = 100;
    s.window.y = 200;
    s.window.width = 1280;
    s.window.height = 720;
    s.window.maximized = true;
    const RestorePlan p = build_restore_plan(s);
    EXPECT_TRUE(p.has_window_geometry);
    EXPECT_EQ(p.window_x, 100);
    EXPECT_EQ(p.window_y, 200);
    EXPECT_EQ(p.window_width, 1280);
    EXPECT_EQ(p.window_height, 720);
    EXPECT_TRUE(p.window_maximized);
}

// ---- テーマ ----

TEST(RestorePlanTest, ThemeNormalization)
{
    EXPECT_EQ(normalize_theme("light"), ThemeKind::Light);
    EXPECT_EQ(normalize_theme("dark"), ThemeKind::Dark);
    EXPECT_EQ(normalize_theme("system"), ThemeKind::System);
    EXPECT_EQ(normalize_theme(""), ThemeKind::System);
    EXPECT_EQ(normalize_theme("unknown"), ThemeKind::System);
}

TEST(RestorePlanTest, ThemeCurrentMappedIntoPlan)
{
    AppState s;
    s.version = kStateVersion;
    s.theme_current = "dark";
    const RestorePlan p = build_restore_plan(s);
    EXPECT_EQ(p.theme, ThemeKind::Dark);
}

// ---- recent のクランプ（最大20件・順序保持） ----

TEST(RestorePlanTest, RecentClampedToLimitPreservingOrder)
{
    AppState s;
    s.version = kStateVersion;
    for (int i = 0; i < 30; ++i)
    {
        s.recent.files.push_back("C:/ws/f" + std::to_string(i) + ".md");
    }
    const RestorePlan p = build_restore_plan(s);
    EXPECT_EQ(p.recent_files.size(), pika::core::state::kRecentLimit);
    // 先頭優先（新しい順前提）でクランプ。先頭が保持される。
    EXPECT_EQ(p.recent_files.front(), "C:/ws/f0.md");
}

// ---- タブ内容の写し ----

TEST(RestorePlanTest, TabFieldsCopiedAndModeNormalized)
{
    AppState s;
    s.version = kStateVersion;
    TabState t;
    t.path = "C:/ws/doc.md";
    t.caret = 42;
    t.scroll = 7;
    t.preview_scroll = 3;
    t.mode = "diff";
    s.tabs.push_back(t);
    const RestorePlan p = build_restore_plan(s);
    ASSERT_EQ(p.tabs.size(), 1u);
    EXPECT_EQ(p.tabs[0].path, "C:/ws/doc.md");
    EXPECT_EQ(p.tabs[0].caret, 42);
    EXPECT_EQ(p.tabs[0].scroll, 7);
    EXPECT_EQ(p.tabs[0].preview_scroll, 3);
    EXPECT_EQ(p.tabs[0].mode, ViewMode::Source);
    EXPECT_TRUE(p.tabs[0].diff_on);
}

// ---- 決定論（同一入力で同一出力） ----

TEST(RestorePlanTest, Deterministic)
{
    AppState s;
    s.version = kStateVersion;
    s.last_workspace = "C:/ws";
    s.tabs.push_back(TabState{"C:/ws/a.md", 1, 2, "preview", 3});
    s.tabs.push_back(TabState{"C:/ws/b.md", 4, 5, "diff", 6});
    s.active_tab = 0;
    s.tree_expanded = {"src", "src/core"};
    s.theme_current = "light";

    const RestorePlan p1 = build_restore_plan(s);
    const RestorePlan p2 = build_restore_plan(s);
    EXPECT_EQ(p1.last_workspace, p2.last_workspace);
    EXPECT_EQ(p1.tabs.size(), p2.tabs.size());
    EXPECT_EQ(p1.active_tab, p2.active_tab);
    EXPECT_EQ(p1.tree_expanded, p2.tree_expanded);
    EXPECT_EQ(p1.theme, p2.theme);
    ASSERT_EQ(p1.tabs.size(), 2u);
    EXPECT_EQ(p1.tabs[1].mode, ViewMode::Source);
    EXPECT_TRUE(p1.tabs[1].diff_on);
}

} // namespace
