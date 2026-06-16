#include "controller/settings_view.h"

namespace pika::controller
{

ViewMode to_view_mode(core::settings::ViewMode m)
{
    switch (m)
    {
    case core::settings::ViewMode::Split:
        return ViewMode::Split;
    case core::settings::ViewMode::Preview:
        return ViewMode::Preview;
    case core::settings::ViewMode::Source:
        return ViewMode::Source;
    case core::settings::ViewMode::Diff:
        // 4モードの Diff は 3モード＋差分トグルへ正規化（ui-design 14章）。モードは Source へ畳む
        // （差分トグル ON は GUI が default_mode 適用時に併せて立てる）。
        return ViewMode::Source;
    }
    return ViewMode::Source; // 防御的フォールバック（網羅済み）。
}

ThemeKind to_theme_kind(core::settings::Theme t)
{
    switch (t)
    {
    case core::settings::Theme::Light:
        return ThemeKind::Light;
    case core::settings::Theme::Dark:
        return ThemeKind::Dark;
    case core::settings::Theme::System:
        return ThemeKind::System;
    }
    return ThemeKind::System; // 防御的フォールバック。
}

UiSettings to_ui_settings(const core::settings::Settings& s)
{
    UiSettings ui;
    ui.exclude = s.exclude;
    ui.big_file_stage1_bytes = s.big_file_stage1_bytes;
    ui.big_file_stage2_bytes = s.big_file_stage2_bytes;
    ui.tab_width = s.tab_width;
    ui.tab_inserts_spaces = s.tab_inserts_spaces;
    ui.show_whitespace = s.show_whitespace;
    ui.word_wrap = s.word_wrap;
    ui.font_family = s.font_family;
    ui.font_size = s.font_size;
    ui.allow_remote_resources = s.allow_remote_resources;
    ui.feature_mermaid = s.feature_mermaid;
    ui.feature_math = s.feature_math;
    ui.feature_code_highlight = s.feature_code_highlight;
    ui.default_mode = to_view_mode(s.default_mode);
    ui.theme = to_theme_kind(s.theme);
    ui.poll_interval_seconds = s.poll_interval_seconds;
    return ui;
}

SettingsApplyResult apply_settings(const core::settings::LoadResult& load)
{
    SettingsApplyResult result;
    // 不正キー名をそのまま透過する（通知バーが「どのキーが不正か」を提示できるように）。
    // warning_count は後方互換のため件数も残す（warning_keys.size() と一致）。
    result.warning_keys = load.warnings;
    result.warning_count = load.warnings.size();
    result.parse_failed = !load.parse_ok;

    // 構文破損（保存途中の不完全 TOML 等）は再適用しない＝直前の有効値を維持する（要件10.3）。
    // settings 自体は load 側で previous が入っているが、UI へは「適用しない」指示を出す
    // （既定値への全戻しでちらつかせない）。
    result.apply = load.parse_ok;
    result.settings = to_ui_settings(load.settings);
    return result;
}

} // namespace pika::controller
