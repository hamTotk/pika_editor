// core/watcher 確定読みの検証（sprint3 must「確定読み」）。
// 静穏期間＋mtime/サイズの安定確認を満たすまで内容を確定読みせず、中途内容で確定しないことを
// 観測する（design.md 5.2）。純ロジックのため stat と時刻を注入して決定論検証する。
#include "core/watcher/settle_reader.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace
{

using pika::core::watcher::FileStat;
using pika::core::watcher::SettleReader;

FileStat st(std::uint64_t size, std::uint64_t mtime)
{
    return FileStat{true, size, mtime};
}

TEST(SettleReaderTest, DoesNotConfirmWhileSizeStillGrowing)
{
    SettleReader r(100, 2);
    // 書き込み継続中: size が伸び続ける＝毎回変化で安定カウントがリセットされる。
    r.observe(st(10, 1), 0);
    EXPECT_FALSE(r.ready_to_read(200)); // 1 回目だけでは確定不可

    r.observe(st(20, 2), 50);
    EXPECT_FALSE(r.ready_to_read(60));  // 変化したので静穏起点が50へ、まだ静穏未達かつ不安定
    EXPECT_FALSE(r.ready_to_read(200)); // size 変化中は安定カウントが満たない
}

TEST(SettleReaderTest, ConfirmsAfterQuietAndStable)
{
    SettleReader r(100, 2);
    r.observe(st(100, 5), 0);  // 初回
    r.observe(st(100, 5), 50); // 同一 size/mtime → 安定カウント 2
    // 最後の変化(0ms)から静穏100ms経過し、安定2回が揃って初めて確定可。
    EXPECT_FALSE(r.ready_to_read(99)); // 静穏未達
    EXPECT_TRUE(r.ready_to_read(100)); // 静穏達成＋安定
}

TEST(SettleReaderTest, MtimeChangeResetsStability)
{
    SettleReader r(100, 2);
    r.observe(st(100, 5), 0);
    r.observe(st(100, 5), 50); // 安定2
    EXPECT_TRUE(r.ready_to_read(150));

    // mtime だけ変わる（同サイズ上書き）→ 静穏起点と安定がリセットされ再度待つ。
    r.observe(st(100, 9), 150);
    EXPECT_FALSE(r.ready_to_read(200)); // 150+100=250 未満かつ安定1
    r.observe(st(100, 9), 200);         // 安定2
    EXPECT_TRUE(r.ready_to_read(250));
}

TEST(SettleReaderTest, MissingFileNeverReady)
{
    SettleReader r(100, 2);
    r.observe(FileStat{false, 0, 0}, 0);
    r.observe(FileStat{false, 0, 0}, 200);
    EXPECT_FALSE(r.ready_to_read(1000)); // 存在しないファイルは確定読み対象外
}

TEST(SettleReaderTest, ResetStartsFreshCycle)
{
    SettleReader r(100, 2);
    r.observe(st(100, 5), 0);
    r.observe(st(100, 5), 50);
    ASSERT_TRUE(r.ready_to_read(200));
    r.reset();
    EXPECT_FALSE(r.ready_to_read(200)); // リセット後は未観測扱い
}

} // namespace
