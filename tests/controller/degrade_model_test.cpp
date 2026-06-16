// controller/degrade_model の検証（sprint8 must#1）。
// - 読み取り専用/権限なし/シンボリックリンク循環/ネットワークドライブ/クラウドプレースホルダ/
//   画像ピクセル数超過の各ケースを「機能縮退＋アプリ継続＋次の一手」へ写す決定論ロジック。
// - 要件12.1（FS 関連）・12.2（画像ガード）・2.2（総ピクセル数ガード）・design 10章 B3。
#include "controller/degrade_model.h"

#include <gtest/gtest.h>

namespace
{

using pika::controller::degrade_kind_label;
using pika::controller::DegradeInput;
using pika::controller::DegradeKind;
using pika::controller::DegradeOutcome;
using pika::controller::next_step_label;
using pika::controller::NextStep;
using pika::controller::resolve_degrade;

// ---- 縮退なし（通常表示） ----

TEST(DegradeModelTest, NoDegradeIsNone)
{
    DegradeInput in;
    const DegradeOutcome out = resolve_degrade(in);
    EXPECT_EQ(out.kind, DegradeKind::None);
    EXPECT_EQ(out.next_step, NextStep::None);
    EXPECT_TRUE(out.can_continue);
    EXPECT_FALSE(out.blocks_content);
}

// ---- 各ケースが対応する縮退種別・次の一手へ写る ----

TEST(DegradeModelTest, AccessDeniedMapsToRetryAndBlocksContent)
{
    DegradeInput in;
    in.access_denied = true;
    const DegradeOutcome out = resolve_degrade(in);
    EXPECT_EQ(out.kind, DegradeKind::AccessDenied);
    EXPECT_EQ(out.next_step, NextStep::RetryOrClose);
    EXPECT_TRUE(out.blocks_content); // 読めない
    EXPECT_TRUE(out.can_continue);   // それでもアプリは継続
}

TEST(DegradeModelTest, SymlinkLoopStopsExpansion)
{
    DegradeInput in;
    in.symlink_loop = true;
    const DegradeOutcome out = resolve_degrade(in);
    EXPECT_EQ(out.kind, DegradeKind::SymlinkLoop);
    EXPECT_TRUE(out.blocks_content); // 枝の展開を打ち切る
    EXPECT_TRUE(out.can_continue);
}

TEST(DegradeModelTest, CloudPlaceholderDefersContent)
{
    DegradeInput in;
    in.cloud_placeholder = true;
    const DegradeOutcome out = resolve_degrade(in);
    EXPECT_EQ(out.kind, DegradeKind::CloudPlaceholder);
    EXPECT_EQ(out.next_step, NextStep::OpenOnDemand);
    // 列挙時点では内容を読まない＝ハイドレーション（全DL事故）を誘発しない（要件12.1）。
    EXPECT_TRUE(out.blocks_content);
    EXPECT_TRUE(out.can_continue);
}

TEST(DegradeModelTest, NetworkDriveDegradesToPollingButContentReadable)
{
    DegradeInput in;
    in.network_drive = true;
    const DegradeOutcome out = resolve_degrade(in);
    EXPECT_EQ(out.kind, DegradeKind::NetworkDrive);
    EXPECT_EQ(out.next_step, NextStep::PollingNotice);
    EXPECT_FALSE(out.blocks_content); // 内容は読める（監視だけポーリングへ縮退）
    EXPECT_TRUE(out.can_continue);
}

TEST(DegradeModelTest, ReadOnlyOpensButGuidesSaveAs)
{
    DegradeInput in;
    in.read_only = true;
    const DegradeOutcome out = resolve_degrade(in);
    EXPECT_EQ(out.kind, DegradeKind::ReadOnly);
    EXPECT_EQ(out.next_step, NextStep::SaveAsOrUnlock);
    EXPECT_FALSE(out.blocks_content); // 開ける（保存時のみ誘導）
    EXPECT_TRUE(out.can_continue);
}

// ---- 画像ピクセル数ガード（要件2.2・12.2） ----

TEST(DegradeModelTest, ImageWithinPixelGuardIsNotDegraded)
{
    DegradeInput in;
    in.is_image = true;
    in.pixel_count = 1920ull * 1080ull; // 約207万px。既定上限6000万px 未満
    const DegradeOutcome out = resolve_degrade(in);
    EXPECT_EQ(out.kind, DegradeKind::None); // 通常表示（デコードしてよい）
    EXPECT_FALSE(out.blocks_content);
}

TEST(DegradeModelTest, ImageOverPixelGuardBlocksDecodeAndOffersExternal)
{
    DegradeInput in;
    in.is_image = true;
    in.pixel_count = 70'000'000ull; // 7000万px。既定上限6000万px 超
    const DegradeOutcome out = resolve_degrade(in);
    EXPECT_EQ(out.kind, DegradeKind::ImageTooLarge);
    EXPECT_EQ(out.next_step, NextStep::OpenInDefaultApp);
    EXPECT_TRUE(out.blocks_content); // デコードしない（固まらない）
    EXPECT_TRUE(out.can_continue);
}

TEST(DegradeModelTest, ImageGuardBoundaryIsStrictlyGreater)
{
    DegradeInput in;
    in.is_image = true;
    in.max_pixels = 100;
    in.pixel_count = 100; // ちょうど上限はデコード可（> でのみ縮退）
    EXPECT_EQ(resolve_degrade(in).kind, DegradeKind::None);
    in.pixel_count = 101; // 1px 超でブロック
    EXPECT_EQ(resolve_degrade(in).kind, DegradeKind::ImageTooLarge);
}

TEST(DegradeModelTest, NonImageIgnoresPixelCount)
{
    DegradeInput in;
    in.is_image = false;
    in.pixel_count = 999'999'999ull; // 画像でなければ無視される
    EXPECT_EQ(resolve_degrade(in).kind, DegradeKind::None);
}

TEST(DegradeModelTest, PixelGuardLimitIsConfigurable)
{
    // 上限を設定で緩和できる（要件2.2「上限はすべて設定で緩和できる」）。
    DegradeInput in;
    in.is_image = true;
    in.pixel_count = 70'000'000ull;
    in.max_pixels = 80'000'000ull; // 緩和後の上限内
    EXPECT_EQ(resolve_degrade(in).kind, DegradeKind::None);
}

// ---- 優先順位（複数同時成立時の解決順） ----

TEST(DegradeModelTest, AccessDeniedTakesPriorityOverEverything)
{
    DegradeInput in;
    in.access_denied = true;
    in.symlink_loop = true;
    in.cloud_placeholder = true;
    in.network_drive = true;
    in.read_only = true;
    in.is_image = true;
    in.pixel_count = 99'999'999ull;
    EXPECT_EQ(resolve_degrade(in).kind, DegradeKind::AccessDenied);
}

TEST(DegradeModelTest, SymlinkLoopBeatsCloudAndImage)
{
    DegradeInput in;
    in.symlink_loop = true;
    in.cloud_placeholder = true;
    in.is_image = true;
    in.pixel_count = 99'999'999ull;
    EXPECT_EQ(resolve_degrade(in).kind, DegradeKind::SymlinkLoop);
}

TEST(DegradeModelTest, CloudBeatsImageAndNetworkAndReadOnly)
{
    DegradeInput in;
    in.cloud_placeholder = true;
    in.is_image = true;
    in.pixel_count = 99'999'999ull;
    in.network_drive = true;
    in.read_only = true;
    EXPECT_EQ(resolve_degrade(in).kind, DegradeKind::CloudPlaceholder);
}

TEST(DegradeModelTest, ImageBeatsNetworkAndReadOnly)
{
    DegradeInput in;
    in.is_image = true;
    in.pixel_count = 99'999'999ull;
    in.network_drive = true;
    in.read_only = true;
    EXPECT_EQ(resolve_degrade(in).kind, DegradeKind::ImageTooLarge);
}

TEST(DegradeModelTest, NetworkBeatsReadOnly)
{
    DegradeInput in;
    in.network_drive = true;
    in.read_only = true;
    EXPECT_EQ(resolve_degrade(in).kind, DegradeKind::NetworkDrive);
}

// ---- 継続性：どの縮退でもアプリはクラッシュ・フリーズしない（要件12.1） ----

TEST(DegradeModelTest, AllDegradesKeepAppRunning)
{
    for (bool ad : {false, true})
        for (bool sl : {false, true})
            for (bool cp : {false, true})
            {
                DegradeInput in;
                in.access_denied = ad;
                in.symlink_loop = sl;
                in.cloud_placeholder = cp;
                EXPECT_TRUE(resolve_degrade(in).can_continue);
            }
}

// ---- 文言（単一メッセージ定義経由・各 enum 値に対応） ----

TEST(DegradeModelTest, LabelsAreNonEmptyForRealKinds)
{
    EXPECT_TRUE(degrade_kind_label(DegradeKind::None).empty());
    EXPECT_FALSE(degrade_kind_label(DegradeKind::AccessDenied).empty());
    EXPECT_FALSE(degrade_kind_label(DegradeKind::CloudPlaceholder).empty());
    EXPECT_FALSE(degrade_kind_label(DegradeKind::ImageTooLarge).empty());
    EXPECT_FALSE(degrade_kind_label(DegradeKind::NetworkDrive).empty());
    EXPECT_FALSE(degrade_kind_label(DegradeKind::ReadOnly).empty());

    EXPECT_TRUE(next_step_label(NextStep::None).empty());
    EXPECT_FALSE(next_step_label(NextStep::OpenInDefaultApp).empty());
    EXPECT_FALSE(next_step_label(NextStep::RetryOrClose).empty());
    EXPECT_FALSE(next_step_label(NextStep::SaveAsOrUnlock).empty());
    EXPECT_FALSE(next_step_label(NextStep::OpenOnDemand).empty());
}

// ---- 決定論：同一入力で同一出力 ----

TEST(DegradeModelTest, Deterministic)
{
    DegradeInput in;
    in.cloud_placeholder = true;
    in.is_image = true;
    in.pixel_count = 70'000'000ull;
    const DegradeOutcome a = resolve_degrade(in);
    const DegradeOutcome b = resolve_degrade(in);
    EXPECT_EQ(a.kind, b.kind);
    EXPECT_EQ(a.next_step, b.next_step);
    EXPECT_EQ(a.blocks_content, b.blocks_content);
    EXPECT_EQ(a.can_continue, b.can_continue);
}

} // namespace
