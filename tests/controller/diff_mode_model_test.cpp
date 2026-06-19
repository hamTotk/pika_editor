// controller/diff_mode_model の検証（sprint5 must）。
// - 直交組合せ (モード ソース/分割/プレビュー) × (差分 ON/OFF) の描画面構成（ui-design 8章）。
// - 占有世代 (タブ, モード, 差分ON) の算定（design 4章・6章）。
// - 差分トグルの自動無効化（10MB超・WebView2不在・ベースライン未取得。ui-design 8/15章）。
#include "controller/diff_mode_model.h"

#include <gtest/gtest.h>

namespace
{

using pika::controller::diff_disable_reason_label;
using pika::controller::diff_toggle_enabled;
using pika::controller::DiffDisableReason;
using pika::controller::DiffToggleContext;
using pika::controller::evaluate_diff_toggle;
using pika::controller::OccupancyKey;
using pika::controller::OccupancyTracker;
using pika::controller::PaneLayout;
using pika::controller::resolve_pane_layout;
using pika::controller::ViewMode;

// ---- 直交組合せ：描画面構成（ui-design 8章の対応表） ----

TEST(DiffModeModelTest, SourceWithoutDiffShowsEditorOnly)
{
    const PaneLayout l = resolve_pane_layout(ViewMode::Source, /*diff_on*/ false);
    EXPECT_TRUE(l.show_editor);
    EXPECT_FALSE(l.show_preview);
    EXPECT_FALSE(l.show_diff);
    EXPECT_FALSE(l.webview_active);
}

TEST(DiffModeModelTest, SourceWithDiffShowsDiffFaceOnly)
{
    // ソース＋差分ON＝差分面のみ（実質差分ビュー）。エディタは出さない。
    const PaneLayout l = resolve_pane_layout(ViewMode::Source, /*diff_on*/ true);
    EXPECT_FALSE(l.show_editor);
    EXPECT_FALSE(l.show_preview);
    EXPECT_TRUE(l.show_diff);
    EXPECT_FALSE(l.preview_diff_grid);
    EXPECT_TRUE(l.webview_active);
}

TEST(DiffModeModelTest, SplitWithoutDiffShowsEditorAndPreview)
{
    const PaneLayout l = resolve_pane_layout(ViewMode::Split, /*diff_on*/ false);
    EXPECT_TRUE(l.show_editor);
    EXPECT_TRUE(l.show_preview);
    EXPECT_FALSE(l.show_diff);
    EXPECT_TRUE(l.webview_active);
}

TEST(DiffModeModelTest, SplitWithDiffUsesPreviewDiffGrid)
{
    // 分割＋差分ON＝プレビュー＋差分と同じ「1枚WebView2内に左プレビュー・右差分の grid」
    // （B11 仕様変更。レビュー用途で生ソースより整形＋差分の対比を採る）。エディタは出さない。
    const PaneLayout l = resolve_pane_layout(ViewMode::Split, /*diff_on*/ true);
    EXPECT_FALSE(l.show_editor);
    EXPECT_TRUE(l.show_preview);
    EXPECT_TRUE(l.show_diff);
    EXPECT_TRUE(l.preview_diff_grid);
    EXPECT_TRUE(l.webview_active);
}

TEST(DiffModeModelTest, PreviewWithoutDiffShowsPreviewOnly)
{
    const PaneLayout l = resolve_pane_layout(ViewMode::Preview, /*diff_on*/ false);
    EXPECT_FALSE(l.show_editor);
    EXPECT_TRUE(l.show_preview);
    EXPECT_FALSE(l.show_diff);
    EXPECT_FALSE(l.preview_diff_grid);
    EXPECT_TRUE(l.webview_active);
}

TEST(DiffModeModelTest, PreviewWithDiffUsesGridInSingleWebView)
{
    // プレビュー＋差分ON＝1枚WebView2内に左プレビュー・右差分を grid（design 6章）。
    const PaneLayout l = resolve_pane_layout(ViewMode::Preview, /*diff_on*/ true);
    EXPECT_FALSE(l.show_editor);
    EXPECT_TRUE(l.show_preview);
    EXPECT_TRUE(l.show_diff);
    EXPECT_TRUE(l.preview_diff_grid);
    EXPECT_TRUE(l.webview_active);
}

// 同一入力で同一出力（純粋）。
TEST(DiffModeModelTest, PaneLayoutIsDeterministic)
{
    const PaneLayout a = resolve_pane_layout(ViewMode::Split, true);
    const PaneLayout b = resolve_pane_layout(ViewMode::Split, true);
    EXPECT_EQ(a.show_editor, b.show_editor);
    EXPECT_EQ(a.show_diff, b.show_diff);
    EXPECT_EQ(a.preview_diff_grid, b.preview_diff_grid);
    EXPECT_EQ(a.webview_active, b.webview_active);
}

// ---- 差分トグルの自動無効化（理由つき。優先順位 種別→WebView2→ベースライン→サイズ） ----

TEST(DiffModeModelTest, DiffToggleEnabledWhenAllConditionsMet)
{
    DiffToggleContext ctx;
    ctx.diffable_type = true;
    ctx.webview_available = true;
    ctx.has_baseline = true;
    ctx.content_bytes = 1024;
    EXPECT_EQ(evaluate_diff_toggle(ctx), DiffDisableReason::None);
    EXPECT_TRUE(diff_toggle_enabled(ctx));
}

TEST(DiffModeModelTest, DiffToggleDisabledForNonDiffableType)
{
    DiffToggleContext ctx;
    ctx.diffable_type = false;
    ctx.webview_available = true;
    ctx.has_baseline = true;
    EXPECT_EQ(evaluate_diff_toggle(ctx), DiffDisableReason::NotDiffableType);
    EXPECT_FALSE(diff_toggle_enabled(ctx));
}

TEST(DiffModeModelTest, DiffToggleDisabledWithoutWebView)
{
    DiffToggleContext ctx;
    ctx.diffable_type = true;
    ctx.webview_available = false;
    ctx.has_baseline = true;
    ctx.content_bytes = 1024;
    EXPECT_EQ(evaluate_diff_toggle(ctx), DiffDisableReason::NoWebView);
}

TEST(DiffModeModelTest, DiffToggleDisabledWithoutBaseline)
{
    DiffToggleContext ctx;
    ctx.diffable_type = true;
    ctx.webview_available = true;
    ctx.has_baseline = false;
    ctx.content_bytes = 1024;
    EXPECT_EQ(evaluate_diff_toggle(ctx), DiffDisableReason::NoBaseline);
}

TEST(DiffModeModelTest, DiffToggleDisabledWhenFileTooLarge)
{
    DiffToggleContext ctx;
    ctx.diffable_type = true;
    ctx.webview_available = true;
    ctx.has_baseline = true;
    ctx.max_diff_bytes = 10u * 1024u * 1024u;
    ctx.content_bytes = ctx.max_diff_bytes + 1; // 10MB 超
    EXPECT_EQ(evaluate_diff_toggle(ctx), DiffDisableReason::FileTooLarge);

    // 境界：ちょうど上限は許可（> 判定なので等しいときは有効）。
    ctx.content_bytes = ctx.max_diff_bytes;
    EXPECT_EQ(evaluate_diff_toggle(ctx), DiffDisableReason::None);
}

// 阻害要因が複数あるとき、優先順位（種別が最優先）の最上位を理由として返す。
TEST(DiffModeModelTest, DisableReasonReportsHighestPriorityCause)
{
    DiffToggleContext ctx;
    ctx.diffable_type = false; // 種別（最優先）
    ctx.webview_available = false;
    ctx.has_baseline = false;
    ctx.content_bytes = 100ull * 1024 * 1024; // サイズも超過
    EXPECT_EQ(evaluate_diff_toggle(ctx), DiffDisableReason::NotDiffableType);
}

// 無効化理由ラベルは理由ごとに異なり、空でない（単一メッセージ定義。design 10章 K9）。
TEST(DiffModeModelTest, DisableReasonLabelsAreDistinctAndNonEmpty)
{
    const DiffDisableReason reasons[] = {
        DiffDisableReason::FileTooLarge, DiffDisableReason::NoWebView,
        DiffDisableReason::NoBaseline, DiffDisableReason::NotDiffableType};
    for (auto r : reasons)
    {
        EXPECT_FALSE(diff_disable_reason_label(r).empty());
    }
    EXPECT_NE(diff_disable_reason_label(DiffDisableReason::NoWebView),
              diff_disable_reason_label(DiffDisableReason::NoBaseline));
}

// ---- 占有世代（タブ, モード, 差分ON） ----

TEST(DiffModeModelTest, OccupancyAdvancesGenerationOnKeyChange)
{
    OccupancyTracker t;
    EXPECT_EQ(t.generation(), 0u);

    EXPECT_TRUE(t.occupy({/*tab*/ 1, ViewMode::Preview, /*diff_on*/ false}));
    EXPECT_EQ(t.generation(), 1u);

    // モードだけ変わる → +1。
    EXPECT_TRUE(t.occupy({1, ViewMode::Split, false}));
    EXPECT_EQ(t.generation(), 2u);

    // 差分トグルだけ変わる → +1（差分ON を切替軸に含める。design 4章）。
    EXPECT_TRUE(t.occupy({1, ViewMode::Split, true}));
    EXPECT_EQ(t.generation(), 3u);

    // タブだけ変わる → +1。
    EXPECT_TRUE(t.occupy({2, ViewMode::Split, true}));
    EXPECT_EQ(t.generation(), 4u);
}

TEST(DiffModeModelTest, OccupancySameKeyDoesNotAdvance)
{
    OccupancyTracker t;
    t.occupy({1, ViewMode::Preview, true});
    const std::uint64_t g = t.generation();
    // 同一鍵への再要求は世代を進めない（再ナビゲートを避ける）。
    EXPECT_FALSE(t.occupy({1, ViewMode::Preview, true}));
    EXPECT_EQ(t.generation(), g);
}

TEST(DiffModeModelTest, IsCurrentRejectsStaleStampOrKey)
{
    OccupancyTracker t;
    t.occupy({1, ViewMode::Preview, false});
    const std::uint64_t stamp = t.generation();
    const OccupancyKey key = t.current();
    EXPECT_TRUE(t.is_current(stamp, key));

    // 別の占有へ切替後、古い stamp の結果は適用しない。
    t.occupy({1, ViewMode::Preview, true});
    EXPECT_FALSE(t.is_current(stamp, key));
    // 新しい世代でも鍵が違えば破棄。
    EXPECT_FALSE(t.is_current(t.generation(), key));
    // 現在の世代＋現在の鍵なら適用可。
    EXPECT_TRUE(t.is_current(t.generation(), t.current()));
}

} // namespace
