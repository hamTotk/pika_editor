// core/settings の検証（sprint8 must）。
// - settings.toml を読み取り専用で読み込み、不正値は既定にフォールバックして警告を返し起動不能に
//   しない
// - 保存途中の不完全な TOML でパース失敗時は直前の有効値を維持する
#include "core/settings/settings.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::settings::default_settings;
using pika::core::settings::load_settings;
using pika::core::settings::NewFileEncoding;
using pika::core::settings::Newline;
using pika::core::settings::Settings;
using pika::core::settings::Theme;
using pika::core::settings::ViewMode;

bool has_warning(const std::vector<std::string>& warnings, const std::string& key)
{
    for (const auto& w : warnings)
    {
        if (w == key)
        {
            return true;
        }
    }
    return false;
}

TEST(SettingsTest, EmptyTomlYieldsDefaults)
{
    auto r = load_settings("");
    EXPECT_TRUE(r.parse_ok);
    EXPECT_TRUE(r.warnings.empty());
    const Settings def = default_settings();
    EXPECT_EQ(r.settings.tab_width, def.tab_width);
    EXPECT_EQ(r.settings.allow_remote_resources, def.allow_remote_resources);
    EXPECT_EQ(r.settings.exclude, def.exclude);
}

TEST(SettingsTest, ValidValuesAreApplied)
{
    const std::string toml = R"(
allowRemoteResources = true
tabWidth = 2
tabInsertsSpaces = true
maxLineLength = 50000
fontFamily = "Cascadia Code"
fontSize = 13
defaultMode = "diff"
newFileEncoding = "shift_jis"
newFileNewline = "lf"
theme = "dark"
exclude = [".git", "node_modules", "dist"]

[unread]
fullHashOnStartup = true

[snapshot]
capacityBytes = 1073741824
sensitivePatterns = ["*.key", "*.token"]

[feature]
mermaid = false
)";
    auto r = load_settings(toml);
    ASSERT_TRUE(r.parse_ok);
    EXPECT_TRUE(r.warnings.empty()) << (r.warnings.empty() ? "" : r.warnings.front());
    EXPECT_TRUE(r.settings.allow_remote_resources);
    EXPECT_EQ(r.settings.tab_width, 2u);
    EXPECT_TRUE(r.settings.tab_inserts_spaces);
    EXPECT_EQ(r.settings.max_line_length, 50000u);
    EXPECT_EQ(r.settings.font_family, "Cascadia Code");
    EXPECT_EQ(r.settings.font_size, 13u);
    EXPECT_EQ(r.settings.default_mode, ViewMode::Diff);
    EXPECT_EQ(r.settings.new_file_encoding, NewFileEncoding::ShiftJis);
    EXPECT_EQ(r.settings.new_file_newline, Newline::Lf);
    EXPECT_EQ(r.settings.theme, Theme::Dark);
    EXPECT_EQ(r.settings.exclude, (std::vector<std::string>{".git", "node_modules", "dist"}));
    EXPECT_TRUE(r.settings.unread_full_hash_on_startup);
    EXPECT_EQ(r.settings.snapshot_capacity_bytes, 1073741824u);
    EXPECT_EQ(r.settings.sensitive_patterns, (std::vector<std::string>{"*.key", "*.token"}));
    EXPECT_FALSE(r.settings.feature_mermaid);
}

TEST(SettingsTest, InvalidTypeFallsBackToDefaultWithWarning)
{
    // tabWidth に文字列、allowRemoteResources に整数を与えると既定へフォールバックし警告を積む。
    const std::string toml = R"(
tabWidth = "wide"
allowRemoteResources = 1
)";
    auto r = load_settings(toml);
    ASSERT_TRUE(r.parse_ok); // 構文は正しい
    const Settings def = default_settings();
    EXPECT_EQ(r.settings.tab_width, def.tab_width);
    EXPECT_EQ(r.settings.allow_remote_resources, def.allow_remote_resources);
    EXPECT_TRUE(has_warning(r.warnings, "tabWidth"));
    EXPECT_TRUE(has_warning(r.warnings, "allowRemoteResources"));
}

TEST(SettingsTest, OutOfRangeFallsBackWithWarning)
{
    // tabWidth は 1〜16、fontSize は 6〜72。範囲外は既定へ戻して警告する（起動不能にしない）。
    const std::string toml = R"(
tabWidth = 999
fontSize = 1000
)";
    auto r = load_settings(toml);
    ASSERT_TRUE(r.parse_ok);
    const Settings def = default_settings();
    EXPECT_EQ(r.settings.tab_width, def.tab_width);
    EXPECT_EQ(r.settings.font_size, def.font_size);
    EXPECT_TRUE(has_warning(r.warnings, "tabWidth"));
    EXPECT_TRUE(has_warning(r.warnings, "fontSize"));
}

TEST(SettingsTest, UnknownEnumFallsBackWithWarning)
{
    auto r = load_settings("defaultMode = \"hologram\"\n");
    ASSERT_TRUE(r.parse_ok);
    EXPECT_EQ(r.settings.default_mode, default_settings().default_mode);
    EXPECT_TRUE(has_warning(r.warnings, "defaultMode"));
}

TEST(SettingsTest, NegativeIntegerFallsBack)
{
    auto r = load_settings("pollIntervalSeconds = -5\n");
    ASSERT_TRUE(r.parse_ok);
    EXPECT_EQ(r.settings.poll_interval_seconds, default_settings().poll_interval_seconds);
    EXPECT_TRUE(has_warning(r.warnings, "pollIntervalSeconds"));
}

TEST(SettingsTest, BrokenTomlKeepsPreviousValid)
{
    // 直前の有効値（remote 許可・タブ 2）を保持し、保存途中の壊れた TOML では既定へ全戻ししない。
    Settings prev = default_settings();
    prev.allow_remote_resources = true;
    prev.tab_width = 2;

    // 閉じていないテーブル・等号欠落で構文破損させる（保存途中の不完全 TOML）。
    const std::string broken = "tabWidth = \nallowRemoteResources = [unterminated";
    auto r = load_settings(broken, prev);

    EXPECT_FALSE(r.parse_ok);
    EXPECT_TRUE(r.warnings.empty());                // 全戻しせず警告でちらつかせない
    EXPECT_TRUE(r.settings.allow_remote_resources); // 直前値を維持
    EXPECT_EQ(r.settings.tab_width, 2u);
}

TEST(SettingsTest, ReadOnlyDoesNotMutateInput)
{
    // 読み取り専用：load は入力文字列を一切変更しない（pika は書き戻さない方針の最小担保）。
    const std::string toml = "tabWidth = 3\n";
    std::string copy = toml;
    auto r = load_settings(copy);
    EXPECT_EQ(copy, toml);
    EXPECT_EQ(r.settings.tab_width, 3u);
}

TEST(SettingsTest, Stage1MustBeBelowStage2)
{
    // 巨大ファイル段階1 が段階2 以上なら整合崩れ。段階2 を既定へ戻して警告する。
    const std::string toml = R"(
[bigFile]
stage1Bytes = 300000000
stage2Bytes = 100000000
)";
    auto r = load_settings(toml);
    ASSERT_TRUE(r.parse_ok);
    EXPECT_LT(r.settings.big_file_stage1_bytes, r.settings.big_file_stage2_bytes);
    EXPECT_TRUE(has_warning(r.warnings, "bigFile.stage2Bytes"));
}

} // namespace
