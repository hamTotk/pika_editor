#include "controller/restore_plan.h"

#include <algorithm>

namespace pika::controller
{

namespace
{

// 最近使った項目の保持上限を controller 側でも尊重する（core/state と同じ要件10.2「最大20件」）。
// core::state::kRecentLimit を直接使い、controller で別定義に散らさない（足さない）。
std::vector<std::string> clamp_recent(const std::vector<std::string>& items)
{
    if (items.size() <= core::state::kRecentLimit)
    {
        return items;
    }
    return std::vector<std::string>(
        items.begin(), items.begin() + static_cast<std::ptrdiff_t>(core::state::kRecentLimit));
}

} // namespace

void normalize_mode(const std::string& mode, ViewMode& out_mode, bool& out_diff_on)
{
    // 既定（不明・空文字）は安全側の Source・差分OFF。
    out_mode = ViewMode::Source;
    out_diff_on = false;

    if (mode == "split")
    {
        out_mode = ViewMode::Split;
    }
    else if (mode == "preview")
    {
        out_mode = ViewMode::Preview;
    }
    else if (mode == "diff")
    {
        // 旧 4モードの "diff" を 3モード＋差分トグルへ正規化（ui-design 14章）。
        // 差分はソース面に差分トグルが乗る最小構成（source + diff_on）。
        out_mode = ViewMode::Source;
        out_diff_on = true;
    }
    // "source" / それ以外は既定（Source・OFF）のまま。
}

ThemeKind normalize_theme(const std::string& theme_current)
{
    if (theme_current == "light")
    {
        return ThemeKind::Light;
    }
    if (theme_current == "dark")
    {
        return ThemeKind::Dark;
    }
    // "system" / 空 / 不明はシステム追従（安全な既定）。
    return ThemeKind::System;
}

RestorePlan build_restore_plan(const core::state::AppState& state)
{
    RestorePlan plan;

    // 未知（新しい）version は復元しない（旧版が新版状態を破壊しない。K2 二重ガード）。
    // load_state も Unsupported で弾くが、本層でも安全側に倒し restorable=false（既定起動）にする。
    if (state.version > core::state::kStateVersion)
    {
        plan.restorable = false;
        return plan;
    }
    plan.restorable = true;

    plan.window_x = state.window.x;
    plan.window_y = state.window.y;
    plan.window_width = state.window.width;
    plan.window_height = state.window.height;
    plan.window_maximized = state.window.maximized;
    // 幅・高さが正のときのみ復元（0 や負は OS 既定に委ねる＝壊れたジオメトリで画面外に出さない）。
    plan.has_window_geometry = state.window.width > 0 && state.window.height > 0;

    plan.last_workspace = state.last_workspace;

    for (const auto& t : state.tabs)
    {
        TabRestore r;
        r.path = t.path;
        r.caret = t.caret;
        r.scroll = t.scroll;
        r.preview_scroll = t.preview_scroll;
        normalize_mode(t.mode, r.mode, r.diff_on);
        plan.tabs.push_back(std::move(r));
    }

    // アクティブタブはタブ範囲へクランプ（範囲外・タブ無しは -1＝アクティブなし）。
    const std::int64_t tab_count = static_cast<std::int64_t>(plan.tabs.size());
    if (tab_count > 0 && state.active_tab >= 0 && state.active_tab < tab_count)
    {
        plan.active_tab = state.active_tab;
    }
    else
    {
        plan.active_tab = -1;
    }

    plan.tree_expanded = state.tree_expanded;
    // tree_pane_collapsed は core AppState に未保持のため安全な既定 false のまま（欠落補完。K2）。
    plan.tree_pane_collapsed = false;

    plan.theme = normalize_theme(state.theme_current);

    plan.recent_files = clamp_recent(state.recent.files);
    plan.recent_folders = clamp_recent(state.recent.folders);

    return plan;
}

} // namespace pika::controller
