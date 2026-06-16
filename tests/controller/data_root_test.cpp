// controller/data_root の検証（sprint2 must「portable.txt 検出による両分岐を観測」）。
// design.md 5.1 手順1・7章 K1 / 要件13章。
#include "controller/data_root.h"

#include <gtest/gtest.h>

namespace
{

using pika::controller::DataRootKind;
using pika::controller::DataRootProbe;
using pika::controller::resolve_data_root;

TEST(DataRootTest, PortableMarkerSelectsExeAdjacentPikaData)
{
    DataRootProbe p;
    p.portable_marker_present = true;
    p.exe_dir = "D:\\apps\\pika";
    p.local_app_data = "C:\\Users\\u\\AppData\\Local"; // 無視されるべき

    auto r = resolve_data_root(p);
    ASSERT_TRUE(r.resolved);
    EXPECT_EQ(r.kind, DataRootKind::Portable);
    EXPECT_EQ(r.path, "D:\\apps\\pika\\pika-data");
}

TEST(DataRootTest, NoMarkerSelectsLocalAppDataPika)
{
    DataRootProbe p;
    p.portable_marker_present = false;
    p.exe_dir = "D:\\apps\\pika"; // 無視されるべき
    p.local_app_data = "C:\\Users\\u\\AppData\\Local";

    auto r = resolve_data_root(p);
    ASSERT_TRUE(r.resolved);
    EXPECT_EQ(r.kind, DataRootKind::LocalAppData);
    EXPECT_EQ(r.path, "C:\\Users\\u\\AppData\\Local\\pika");
}

TEST(DataRootTest, ForwardSlashesAndTrailingSeparatorsNormalized)
{
    // 区切り混在・末尾区切りは正規化される（'\\' 統一・末尾畳み）。
    DataRootProbe p;
    p.portable_marker_present = true;
    p.exe_dir = "D:/apps/pika/";

    auto r = resolve_data_root(p);
    ASSERT_TRUE(r.resolved);
    EXPECT_EQ(r.path, "D:\\apps\\pika\\pika-data");
}

TEST(DataRootTest, MissingLocalAppDataIsUnresolved)
{
    // 通常版なのに %LOCALAPPDATA% が取れない → 退避先未確定で解決失敗（データを失わない原則）。
    DataRootProbe p;
    p.portable_marker_present = false;
    p.local_app_data = "";

    auto r = resolve_data_root(p);
    EXPECT_FALSE(r.resolved);
}

TEST(DataRootTest, PortableWithoutExeDirIsUnresolved)
{
    DataRootProbe p;
    p.portable_marker_present = true;
    p.exe_dir = "";

    auto r = resolve_data_root(p);
    EXPECT_FALSE(r.resolved);
}

} // namespace
