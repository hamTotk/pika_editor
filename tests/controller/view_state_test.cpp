// controller/view_state の検証（sprint8 must#2）。
// - ビュー別5状態（Ideal/Empty/Loading/Partial/Error）の決定論解決。
// - Empty の3分岐（フォルダ未オープン/検索0件/消化後）で文言が変わること（ui-design
// 15章・要件10章）。
// - Partial（機能縮退）と Error（致命的に表示不能）の区別（ui-design 15章末尾）。
#include "controller/view_state.h"

#include "controller/degrade_model.h"

#include <gtest/gtest.h>

namespace
{

using pika::controller::DegradeInput;
using pika::controller::DegradeKind;
using pika::controller::empty_reason_label;
using pika::controller::EmptyReason;
using pika::controller::NextStep;
using pika::controller::resolve_degrade;
using pika::controller::resolve_view_state;
using pika::controller::ViewState;
using pika::controller::ViewStateInput;
using pika::controller::ViewStateResult;

// 縮退入力を解決して degrade フィールドに載せるヘルパ（degrade_model と整合させる）。
pika::controller::DegradeOutcome degrade_of(const DegradeInput& in)
{
    return resolve_degrade(in);
}

// ---- Ideal（通常表示） ----

TEST(ViewStateTest, IdealWhenFolderOpenedAndItemsVisible)
{
    ViewStateInput in;
    in.folder_opened = true;
    in.has_visible_items = true;
    EXPECT_EQ(resolve_view_state(in).state, ViewState::Ideal);
}

// ---- Empty の3分岐（文言が変わる） ----

TEST(ViewStateTest, EmptyNoFolderOpened)
{
    ViewStateInput in;
    in.folder_opened = false;
    in.has_visible_items = false;
    const ViewStateResult r = resolve_view_state(in);
    EXPECT_EQ(r.state, ViewState::Empty);
    EXPECT_EQ(r.empty_reason, EmptyReason::NoFolderOpened);
}

TEST(ViewStateTest, EmptySearchNoHits)
{
    ViewStateInput in;
    in.folder_opened = true;
    in.has_visible_items = false;
    in.is_search_mode = true;
    const ViewStateResult r = resolve_view_state(in);
    EXPECT_EQ(r.state, ViewState::Empty);
    EXPECT_EQ(r.empty_reason, EmptyReason::SearchNoHits);
}

TEST(ViewStateTest, EmptyAllConsumed)
{
    ViewStateInput in;
    in.folder_opened = true;
    in.has_visible_items = false;
    in.is_search_mode = false;
    in.all_consumed = true;
    const ViewStateResult r = resolve_view_state(in);
    EXPECT_EQ(r.state, ViewState::Empty);
    EXPECT_EQ(r.empty_reason, EmptyReason::AllConsumed);
}

TEST(ViewStateTest, EmptyThreeBranchesHaveDistinctLabels)
{
    // 「3分岐で文言を変える」（ui-design 15章・要件10章）を観測する。
    const auto no_folder = empty_reason_label(EmptyReason::NoFolderOpened);
    const auto no_hits = empty_reason_label(EmptyReason::SearchNoHits);
    const auto consumed = empty_reason_label(EmptyReason::AllConsumed);
    EXPECT_FALSE(no_folder.empty());
    EXPECT_FALSE(no_hits.empty());
    EXPECT_FALSE(consumed.empty());
    EXPECT_NE(no_folder, no_hits);
    EXPECT_NE(no_hits, consumed);
    EXPECT_NE(no_folder, consumed);
}

// ---- Loading（列挙中・ベースライン取得中。進捗を透過） ----

TEST(ViewStateTest, LoadingCarriesProgress)
{
    ViewStateInput in;
    in.folder_opened = true;
    in.has_visible_items = true; // 逐次追加で一部見えていても列挙中なら Loading
    in.loading = true;
    in.loaded_count = 620;
    in.total_count = 1000;
    const ViewStateResult r = resolve_view_state(in);
    EXPECT_EQ(r.state, ViewState::Loading);
    EXPECT_EQ(r.loaded_count, 620u);
    EXPECT_EQ(r.total_count, 1000u);
}

// ---- Partial（機能縮退）と Error（致命的に表示不能）の区別 ----

TEST(ViewStateTest, PartialWhenDiffAutoOff)
{
    ViewStateInput in;
    in.folder_opened = true;
    in.has_visible_items = true;
    in.diff_auto_off = true; // 10MB超等で差分自動オフ（ui-design 15章 Partial）
    EXPECT_EQ(resolve_view_state(in).state, ViewState::Partial);
}

TEST(ViewStateTest, PartialWhenBaselinePending)
{
    ViewStateInput in;
    in.folder_opened = true;
    in.has_visible_items = true;
    in.baseline_pending = true; // ベースライン未取得のファイルを開いた中間状態（ui-design 15章）
    EXPECT_EQ(resolve_view_state(in).state, ViewState::Partial);
}

TEST(ViewStateTest, PartialWhenImageTooLarge)
{
    DegradeInput d;
    d.is_image = true;
    d.pixel_count = 70'000'000ull;
    ViewStateInput in;
    in.folder_opened = true;
    in.has_visible_items = true;
    in.degrade = degrade_of(d);
    const ViewStateResult r = resolve_view_state(in);
    EXPECT_EQ(r.state, ViewState::Partial);
    EXPECT_EQ(r.degrade_kind, DegradeKind::ImageTooLarge);
    EXPECT_EQ(r.next_step, NextStep::OpenInDefaultApp); // 次の一手が伝わる
}

TEST(ViewStateTest, PartialWhenCloudPlaceholder)
{
    DegradeInput d;
    d.cloud_placeholder = true;
    ViewStateInput in;
    in.folder_opened = true;
    in.has_visible_items = true;
    in.degrade = degrade_of(d);
    EXPECT_EQ(resolve_view_state(in).state, ViewState::Partial);
}

TEST(ViewStateTest, ErrorWhenAccessDenied)
{
    DegradeInput d;
    d.access_denied = true;
    ViewStateInput in;
    in.folder_opened = true;
    in.has_visible_items = true;
    in.degrade = degrade_of(d);
    const ViewStateResult r = resolve_view_state(in);
    EXPECT_EQ(r.state, ViewState::Error); // 読み込み不能＝Error（Partial ではない）
    EXPECT_EQ(r.degrade_kind, DegradeKind::AccessDenied);
    EXPECT_EQ(r.next_step, NextStep::RetryOrClose);
}

TEST(ViewStateTest, NetworkDriveDoesNotForceNonIdeal)
{
    // ネットワークドライブは監視がポーリングへ縮退するだけで本文は通常表示＝Ideal を妨げない。
    DegradeInput d;
    d.network_drive = true;
    ViewStateInput in;
    in.folder_opened = true;
    in.has_visible_items = true;
    in.degrade = degrade_of(d);
    EXPECT_EQ(resolve_view_state(in).state, ViewState::Ideal);
}

TEST(ViewStateTest, ReadOnlyDoesNotForceNonIdeal)
{
    // 読み取り専用は開けて通常表示＝Ideal。保存時の誘導は通知バー側で扱う。
    DegradeInput d;
    d.read_only = true;
    ViewStateInput in;
    in.folder_opened = true;
    in.has_visible_items = true;
    in.degrade = degrade_of(d);
    EXPECT_EQ(resolve_view_state(in).state, ViewState::Ideal);
}

// ---- 上書き優先（Error ＞ Partial ＞ Loading ＞ Empty ＞ Ideal） ----

TEST(ViewStateTest, ErrorOverridesPartialAndLoadingAndEmpty)
{
    DegradeInput d;
    d.access_denied = true;
    ViewStateInput in;
    in.folder_opened = false; // Empty 条件
    in.has_visible_items = false;
    in.loading = true;       // Loading 条件
    in.diff_auto_off = true; // Partial 条件
    in.degrade = degrade_of(d);
    EXPECT_EQ(resolve_view_state(in).state, ViewState::Error);
}

TEST(ViewStateTest, PartialOverridesLoadingAndEmpty)
{
    ViewStateInput in;
    in.folder_opened = false;
    in.has_visible_items = false;
    in.loading = true;
    in.baseline_pending = true; // Partial 条件
    EXPECT_EQ(resolve_view_state(in).state, ViewState::Partial);
}

TEST(ViewStateTest, LoadingOverridesEmpty)
{
    ViewStateInput in;
    in.folder_opened = true;
    in.has_visible_items = false; // Empty 条件
    in.loading = true;
    EXPECT_EQ(resolve_view_state(in).state, ViewState::Loading);
}

// ---- 決定論：同一入力で同一出力 ----

TEST(ViewStateTest, Deterministic)
{
    ViewStateInput in;
    in.folder_opened = true;
    in.has_visible_items = false;
    in.is_search_mode = true;
    const ViewStateResult a = resolve_view_state(in);
    const ViewStateResult b = resolve_view_state(in);
    EXPECT_EQ(a.state, b.state);
    EXPECT_EQ(a.empty_reason, b.empty_reason);
}

} // namespace
