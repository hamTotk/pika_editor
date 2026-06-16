// controller/notification_model の検証（sprint7 must#2）。
// - 最大3本＋「他N件」集約（overflow）。
// - 優先順位（衝突 ＞ 設定エラー ＞ 外部リソース参照 ＞ JS検知 ＞ 巨大ファイル。design 10章 J1）。
// - 同一ファイル・同一種別は最新（seq 最大）の 1 件へ集約。
// - タブ固有/グローバルの切替（アクティブタブ＋グローバルのみ対象・他タブ固有は除外）。
#include "controller/notification_model.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

using pika::controller::aggregate_notifications;
using pika::controller::kMaxVisible;
using pika::controller::Notification;
using pika::controller::NotificationKind;
using pika::controller::NotificationView;

Notification make(NotificationKind kind, const std::string& tab_path, std::uint64_t seq)
{
    Notification n;
    n.kind = kind;
    n.tab_path = tab_path;
    n.seq = seq;
    return n;
}

// ---- 優先順位（design 10章 J1） ----

TEST(NotificationModelTest, PriorityOrdersByKind)
{
    // 投入順をバラバラにしても種別優先順位で整列する。
    std::vector<Notification> in = {
        make(NotificationKind::BigFile, "", 1),        make(NotificationKind::Conflict, "", 2),
        make(NotificationKind::JsDetected, "", 3),     make(NotificationKind::SettingsError, "", 4),
        make(NotificationKind::RemoteResource, "", 5),
    };
    const NotificationView v = aggregate_notifications(in, "");
    // 5件のうち先頭3本が高優先順に並ぶ：Conflict > SettingsError > RemoteResource。
    ASSERT_EQ(v.rows.size(), kMaxVisible);
    EXPECT_EQ(v.rows[0].kind, NotificationKind::Conflict);
    EXPECT_EQ(v.rows[1].kind, NotificationKind::SettingsError);
    EXPECT_EQ(v.rows[2].kind, NotificationKind::RemoteResource);
    EXPECT_EQ(v.overflow, 2u); // JsDetected と BigFile が「他2件」へ畳まれる
}

TEST(NotificationModelTest, ConflictAlwaysHighestPriority)
{
    std::vector<Notification> in = {
        make(NotificationKind::BigFile, "", 100),
        make(NotificationKind::Conflict, "", 1), // 古い衝突でも最優先
    };
    const NotificationView v = aggregate_notifications(in, "");
    ASSERT_GE(v.rows.size(), 1u);
    EXPECT_EQ(v.rows[0].kind, NotificationKind::Conflict);
}

// ---- 同種別内は新しい（seq 大）を優先 ----

TEST(NotificationModelTest, SameKindSameKeyAggregatedToLatestGlobal)
{
    // 同種別・同一キー（グローバル＝tab_path 空）は最新（seq 最大）の 1 件へ集約される。
    std::vector<Notification> in = {
        make(NotificationKind::RemoteResource, "", 1),
        make(NotificationKind::RemoteResource, "", 9),
        make(NotificationKind::RemoteResource, "", 5),
    };
    const NotificationView v = aggregate_notifications(in, "");
    ASSERT_EQ(v.rows.size(), 1u);
    EXPECT_EQ(v.rows[0].seq, 9u); // 最新が残る
    EXPECT_EQ(v.overflow, 0u);
}

// ---- 同一ファイル・同一種別の最新集約 ----

TEST(NotificationModelTest, SameFileSameKindAggregatedToLatest)
{
    std::vector<Notification> in = {
        make(NotificationKind::RemoteResource, "C:/ws/a.md", 1),
        make(NotificationKind::RemoteResource, "C:/ws/a.md", 7), // 同ファイル・同種別の最新
        make(NotificationKind::RemoteResource, "C:/ws/a.md", 4),
    };
    const NotificationView v = aggregate_notifications(in, "C:/ws/a.md");
    ASSERT_EQ(v.rows.size(), 1u);
    EXPECT_EQ(v.rows[0].seq, 7u);
    EXPECT_EQ(v.overflow, 0u);
}

TEST(NotificationModelTest, SameFileDifferentKindsNotAggregated)
{
    // 同ファイルでも種別が違えば別行（集約しない）。
    std::vector<Notification> in = {
        make(NotificationKind::RemoteResource, "C:/ws/a.md", 1),
        make(NotificationKind::JsDetected, "C:/ws/a.md", 2),
    };
    const NotificationView v = aggregate_notifications(in, "C:/ws/a.md");
    EXPECT_EQ(v.rows.size(), 2u);
}

// ---- タブ固有/グローバルの切替 ----

TEST(NotificationModelTest, OtherTabSpecificNotificationsExcluded)
{
    std::vector<Notification> in = {
        make(NotificationKind::RemoteResource, "C:/ws/active.md", 1),
        make(NotificationKind::RemoteResource, "C:/ws/other.md", 2), // 非アクティブタブ固有→除外
        make(NotificationKind::SettingsError, "", 3),                // グローバル→常に対象
    };
    const NotificationView v = aggregate_notifications(in, "C:/ws/active.md");
    ASSERT_EQ(v.rows.size(), 2u);
    // SettingsError(グローバル) が優先順位で上、RemoteResource(アクティブタブ固有) が下。
    EXPECT_EQ(v.rows[0].kind, NotificationKind::SettingsError);
    EXPECT_TRUE(v.rows[0].is_global);
    EXPECT_EQ(v.rows[1].kind, NotificationKind::RemoteResource);
    EXPECT_FALSE(v.rows[1].is_global);
    EXPECT_EQ(v.rows[1].tab_path, "C:/ws/active.md");
}

TEST(NotificationModelTest, GlobalNotificationsShownRegardlessOfActiveTab)
{
    std::vector<Notification> in = {
        make(NotificationKind::SettingsError, "", 1),
    };
    // アクティブタブが何であれグローバルは出る。
    const NotificationView v1 = aggregate_notifications(in, "C:/ws/x.md");
    const NotificationView v2 = aggregate_notifications(in, "");
    ASSERT_EQ(v1.rows.size(), 1u);
    ASSERT_EQ(v2.rows.size(), 1u);
    EXPECT_TRUE(v1.rows[0].is_global);
    EXPECT_TRUE(v2.rows[0].is_global);
}

// ---- 最大3本＋他N件 ----

TEST(NotificationModelTest, OverflowBeyondThreeRows)
{
    // 5 種別すべて別行 → 先頭3本＋他2件。
    std::vector<Notification> in = {
        make(NotificationKind::Conflict, "", 1),       make(NotificationKind::SettingsError, "", 2),
        make(NotificationKind::RemoteResource, "", 3), make(NotificationKind::JsDetected, "", 4),
        make(NotificationKind::BigFile, "", 5),
    };
    const NotificationView v = aggregate_notifications(in, "");
    EXPECT_EQ(v.rows.size(), kMaxVisible);
    EXPECT_EQ(v.overflow, 2u);
}

TEST(NotificationModelTest, ExactlyThreeRowsNoOverflow)
{
    std::vector<Notification> in = {
        make(NotificationKind::Conflict, "", 1),
        make(NotificationKind::SettingsError, "", 2),
        make(NotificationKind::RemoteResource, "", 3),
    };
    const NotificationView v = aggregate_notifications(in, "");
    EXPECT_EQ(v.rows.size(), 3u);
    EXPECT_EQ(v.overflow, 0u);
}

TEST(NotificationModelTest, EmptyInputProducesEmptyView)
{
    const NotificationView v = aggregate_notifications({}, "C:/ws/a.md");
    EXPECT_TRUE(v.rows.empty());
    EXPECT_EQ(v.overflow, 0u);
}

// ---- 決定論 ----

TEST(NotificationModelTest, Deterministic)
{
    std::vector<Notification> in = {
        make(NotificationKind::BigFile, "C:/ws/a.md", 1),
        make(NotificationKind::Conflict, "C:/ws/a.md", 2),
        make(NotificationKind::RemoteResource, "", 3),
        make(NotificationKind::RemoteResource, "", 8),
    };
    const NotificationView v1 = aggregate_notifications(in, "C:/ws/a.md");
    const NotificationView v2 = aggregate_notifications(in, "C:/ws/a.md");
    ASSERT_EQ(v1.rows.size(), v2.rows.size());
    for (std::size_t i = 0; i < v1.rows.size(); ++i)
    {
        EXPECT_EQ(v1.rows[i].kind, v2.rows[i].kind);
        EXPECT_EQ(v1.rows[i].seq, v2.rows[i].seq);
    }
    EXPECT_EQ(v1.overflow, v2.overflow);
}

} // namespace
