// controller/settings_view の検証（sprint7 must#4）。
// - core/settings::load_settings の LoadResult を UI 設定へ写す純粋写像。
// - parse_ok=false（構文破損）は apply=false（直前の有効値を維持。ちらつかせない。要件10.3）。
// - 不正値の warnings 件数を UI へ伝える（通知バー SettingsError 用）。
// - pika は書き戻さない（読み取り専用＝入力 LoadResult は不変）。
// - enum 語彙の写し替え（core::settings::ViewMode 4モード → controller::ViewMode 3モード・Theme）。
#include "controller/settings_view.h"

#include "core/settings/settings.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace
{

using pika::controller::apply_settings;
using pika::controller::SettingsApplyResult;
using pika::controller::ThemeKind;
using pika::controller::to_theme_kind;
using pika::controller::to_ui_settings;
using pika::controller::to_view_mode;
using pika::controller::UiSettings;
using pika::controller::ViewMode;
using pika::core::settings::load_settings;
using pika::core::settings::Settings;

// ---- enum 語彙の写し替え ----

TEST(SettingsViewTest, ViewModeMapping)
{
    EXPECT_EQ(to_view_mode(pika::core::settings::ViewMode::Split), ViewMode::Split);
    EXPECT_EQ(to_view_mode(pika::core::settings::ViewMode::Preview), ViewMode::Preview);
    EXPECT_EQ(to_view_mode(pika::core::settings::ViewMode::Source), ViewMode::Source);
    // 4モードの Diff は 3モード＋差分トグルへ正規化＝モードは Source へ畳む（ui-design 14章）。
    EXPECT_EQ(to_view_mode(pika::core::settings::ViewMode::Diff), ViewMode::Source);
}

TEST(SettingsViewTest, ThemeMapping)
{
    EXPECT_EQ(to_theme_kind(pika::core::settings::Theme::Light), ThemeKind::Light);
    EXPECT_EQ(to_theme_kind(pika::core::settings::Theme::Dark), ThemeKind::Dark);
    EXPECT_EQ(to_theme_kind(pika::core::settings::Theme::System), ThemeKind::System);
}

// ---- Settings → UiSettings の写し ----

TEST(SettingsViewTest, ToUiSettingsCopiesFields)
{
    Settings s;
    s.tab_width = 2;
    s.tab_inserts_spaces = true;
    s.word_wrap = true;
    s.font_family = "Meiryo";
    s.font_size = 13;
    s.allow_remote_resources = true;
    s.feature_mermaid = false;
    s.theme = pika::core::settings::Theme::Dark;
    s.default_mode = pika::core::settings::ViewMode::Preview;
    s.poll_interval_seconds = 10;

    const UiSettings ui = to_ui_settings(s);
    EXPECT_EQ(ui.tab_width, 2u);
    EXPECT_TRUE(ui.tab_inserts_spaces);
    EXPECT_TRUE(ui.word_wrap);
    EXPECT_EQ(ui.font_family, "Meiryo");
    EXPECT_EQ(ui.font_size, 13u);
    EXPECT_TRUE(ui.allow_remote_resources);
    EXPECT_FALSE(ui.feature_mermaid);
    EXPECT_EQ(ui.theme, ThemeKind::Dark);
    EXPECT_EQ(ui.default_mode, ViewMode::Preview);
    EXPECT_EQ(ui.poll_interval_seconds, 10u);
}

// ---- apply_settings：正常 TOML ----

TEST(SettingsViewTest, ApplyValidSettingsAppliesAndNoWarnings)
{
    const auto load = load_settings("tabWidth = 2\nwordWrap = true\n");
    const SettingsApplyResult r = apply_settings(load);
    EXPECT_TRUE(r.apply);
    EXPECT_FALSE(r.parse_failed);
    EXPECT_EQ(r.warning_count, 0u);
    EXPECT_EQ(r.settings.tab_width, 2u);
    EXPECT_TRUE(r.settings.word_wrap);
}

// ---- apply_settings：不正値で warnings 件数を伝える ----

TEST(SettingsViewTest, ApplyInvalidValueSurfacesWarningCount)
{
    // 型違い（tabWidth に文字列）は load_settings が既定へ丸めて warnings に積む。
    const auto load = load_settings("tabWidth = \"oops\"\n");
    ASSERT_TRUE(load.parse_ok); // 構文は正しい
    ASSERT_FALSE(load.warnings.empty());

    const SettingsApplyResult r = apply_settings(load);
    EXPECT_TRUE(r.apply); // 構文 OK なので反映する（不正値は既定に丸め済み）
    EXPECT_FALSE(r.parse_failed);
    EXPECT_EQ(r.warning_count, load.warnings.size());
    EXPECT_GT(r.warning_count, 0u);
}

// ---- apply_settings：不正キー名を UI へ透過する（warning_keys） ----

TEST(SettingsViewTest, ApplyInvalidKeysSurfaceWarningKeyNames)
{
    using pika::core::settings::LoadResult;
    // 複数の不正キーを含む LoadResult を直接渡し、キー名がそのまま透過されることを検証する
    // （通知バー集約が「どのキーが不正か」を提示できる素材になる）。
    LoadResult load;
    load.parse_ok = true;
    load.warnings = {"tabWidth", "pollIntervalSeconds", "fontSize"};

    const SettingsApplyResult r = apply_settings(load);
    EXPECT_TRUE(r.apply); // 構文 OK＝反映する（不正値は既定へ丸め済み）
    // warning_keys にキー名が順序を保って保持されている。
    ASSERT_EQ(r.warning_keys.size(), 3u);
    EXPECT_EQ(r.warning_keys[0], "tabWidth");
    EXPECT_EQ(r.warning_keys[1], "pollIntervalSeconds");
    EXPECT_EQ(r.warning_keys[2], "fontSize");
    // 後方互換：件数は keys のサイズと一致する。
    EXPECT_EQ(r.warning_count, r.warning_keys.size());
}

TEST(SettingsViewTest, ApplyValidSettingsHasEmptyWarningKeys)
{
    const auto load = load_settings("tabWidth = 2\n");
    const SettingsApplyResult r = apply_settings(load);
    EXPECT_TRUE(r.warning_keys.empty());
    EXPECT_EQ(r.warning_count, 0u);
}

// ---- apply_settings：実 TOML 由来の不正キーも warning_keys に乗る ----

TEST(SettingsViewTest, ApplyInvalidValueFromTomlSurfacesKeyNames)
{
    // 型違い（tabWidth に文字列）は load_settings が既定へ丸めて warnings に積む。
    const auto load = load_settings("tabWidth = \"oops\"\n");
    ASSERT_TRUE(load.parse_ok);
    ASSERT_FALSE(load.warnings.empty());

    const SettingsApplyResult r = apply_settings(load);
    // load 由来の warnings（キー名）がそのまま透過されている。
    EXPECT_EQ(r.warning_keys, load.warnings);
    EXPECT_EQ(r.warning_count, load.warnings.size());
}

// ---- apply_settings：構文破損は再適用しない（直前値維持） ----

TEST(SettingsViewTest, ApplyBrokenTomlDoesNotReapply)
{
    Settings prev;
    prev.tab_width = 7;
    const auto load = load_settings("this is = = not valid toml [[", prev);
    ASSERT_FALSE(load.parse_ok); // 構文破損

    const SettingsApplyResult r = apply_settings(load);
    // 構文破損は UI へ「適用しない」指示＝直前の有効値を維持（ちらつかせない。要件10.3）。
    EXPECT_FALSE(r.apply);
    EXPECT_TRUE(r.parse_failed);
}

// ---- 読み取り専用：apply_settings は core の Settings を変更しない（純粋写像） ----

TEST(SettingsViewTest, ApplyDoesNotMutateLoadResult)
{
    auto load = load_settings("tabWidth = 3\n");
    const std::uint64_t before = load.settings.tab_width;
    const SettingsApplyResult r = apply_settings(load);
    EXPECT_EQ(load.settings.tab_width, before); // 入力は不変（書き戻さない）
    EXPECT_EQ(r.settings.tab_width, before);
}

} // namespace
