#include "controller/diff_mode_model.h"

namespace pika::controller
{

PaneLayout resolve_pane_layout(ViewMode mode, bool diff_on)
{
    PaneLayout out;
    switch (mode)
    {
    case ViewMode::Source:
        if (diff_on)
        {
            // ソース＋差分ON＝差分面のみ（実質差分ビュー。ui-design 8章）。
            out.show_diff = true;
            out.webview_active = true;
        }
        else
        {
            out.show_editor = true;
        }
        break;

    case ViewMode::Split:
        if (diff_on)
        {
            // 分割＋差分ON＝プレビュー＋差分と同じ「1枚WebView2内に左プレビュー・右差分の grid」
            // へ倒す（B11 仕様変更）。レビュー用途では生ソースより整形プレビューと差分の対比が要る
            // ため、エディタは出さずプレビュー＋差分経路へ合流する（2つ目の WebView2 を作らない）。
            out.show_preview = true;
            out.show_diff = true;
            out.preview_diff_grid = true;
        }
        else
        {
            // 分割＋差分OFF は従来どおり「左＝エディタ／右＝整形プレビュー」を維持する。
            out.show_editor = true;
            out.show_preview = true;
        }
        out.webview_active = true;
        break;

    case ViewMode::Preview:
        if (diff_on)
        {
            // プレビュー＋差分ON＝1枚WebView2内に左プレビュー・右差分を grid（design 6章）。
            out.show_preview = true;
            out.show_diff = true;
            out.preview_diff_grid = true;
        }
        else
        {
            out.show_preview = true;
        }
        out.webview_active = true;
        break;
    }
    return out;
}

DiffDisableReason evaluate_diff_toggle(const DiffToggleContext& ctx)
{
    // 優先順位は「種別 → WebView2 → ベースライン → サイズ」（理由表示は最上位の阻害要因を返す）。
    if (!ctx.diffable_type)
    {
        return DiffDisableReason::NotDiffableType;
    }
    if (!ctx.webview_available)
    {
        return DiffDisableReason::NoWebView;
    }
    if (!ctx.has_baseline)
    {
        return DiffDisableReason::NoBaseline;
    }
    if (ctx.content_bytes > ctx.max_diff_bytes)
    {
        return DiffDisableReason::FileTooLarge;
    }
    return DiffDisableReason::None;
}

bool diff_toggle_enabled(const DiffToggleContext& ctx)
{
    return evaluate_diff_toggle(ctx) == DiffDisableReason::None;
}

std::string_view diff_disable_reason_label(DiffDisableReason reason)
{
    switch (reason)
    {
    case DiffDisableReason::None:
        return "差分を表示できます";
    case DiffDisableReason::FileTooLarge:
        return "ファイルが大きいため差分は自動的にオフです";
    case DiffDisableReason::NoWebView:
        return "WebView2 ランタイムが見つからないため差分を表示できません";
    case DiffDisableReason::NoBaseline:
        return "前回確認時点がないため差分を表示できません";
    case DiffDisableReason::NotDiffableType:
        return "このファイルでは差分を表示できません";
    }
    return "";
}

bool OccupancyTracker::occupy(const OccupancyKey& key)
{
    if (key == current_)
    {
        // 同一占有への再要求は世代を進めない（再ナビゲートを避ける。design 6章 (2) キャッシュ）。
        return false;
    }
    current_ = key;
    ++generation_;
    return true;
}

bool OccupancyTracker::is_current(std::uint64_t stamp, const OccupancyKey& key) const noexcept
{
    return stamp == generation_ && key == current_;
}

} // namespace pika::controller
