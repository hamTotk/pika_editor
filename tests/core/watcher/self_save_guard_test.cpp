// core/watcher 自己保存抑制の検証（sprint3 must「自己保存抑制」）。
// 保存トークン（パス+保存後ハッシュ）に対し、現ディスク内容のハッシュが一致する場合のみ自己イベント
// としてワンショット消費し、時刻窓を超えてもハッシュ一致なら抑制、内容が異なれば外部変更として扱う
// ことを観測する（design.md 5.2）。純ロジックのため決定論検証する。
#include "core/watcher/self_save_guard.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::watcher::SelfSaveGuard;

TEST(SelfSaveGuardTest, ConsumesSelfWhenHashMatches)
{
    SelfSaveGuard g(5000);
    g.register_save("a.md", 0xABCD, 0);
    // ディスク内容のハッシュが保存後ハッシュと一致＝自己保存。
    EXPECT_TRUE(g.consume_if_self("a.md", 0xABCD, 1));
    // ワンショット消費後は同じイベントが来ても抑制しない（外部変更扱い）。
    EXPECT_FALSE(g.consume_if_self("a.md", 0xABCD, 2));
}

TEST(SelfSaveGuardTest, ExternalChangeNotSuppressed)
{
    SelfSaveGuard g(5000);
    g.register_save("a.md", 0xABCD, 0);
    // ディスク内容が保存後ハッシュと異なる＝外部変更。抑制せずトークンは保持される。
    EXPECT_FALSE(g.consume_if_self("a.md", 0x1111, 1));
    EXPECT_EQ(g.pending_count(), 1u);
    // 後で正しい内容（=保存後ハッシュ）が確定すれば自己保存として消費できる。
    EXPECT_TRUE(g.consume_if_self("a.md", 0xABCD, 2));
}

TEST(SelfSaveGuardTest, SuppressesEvenBeyondTimeWindowWhenHashMatches)
{
    SelfSaveGuard g(100); // 100ms 窓
    g.register_save("a.md", 0xABCD, 0);
    // 時刻窓(100ms)を大きく超えても、ハッシュが一致すれば抑制する（窓は補助・主条件はハッシュ）。
    EXPECT_TRUE(g.consume_if_self("a.md", 0xABCD, 100000));
}

TEST(SelfSaveGuardTest, ZeroHashSaveIsNotRegistered)
{
    SelfSaveGuard g(5000);
    // 保存後ハッシュが取れない(0)は登録しない＝後続は外部変更扱い（design.md 5.2）。
    g.register_save("a.md", 0, 0);
    EXPECT_EQ(g.pending_count(), 0u);
    EXPECT_FALSE(g.consume_if_self("a.md", 0, 1));
}

TEST(SelfSaveGuardTest, MultipleSavesConsumeOneAtATime)
{
    SelfSaveGuard g(5000);
    // 同一パスへ 2 回保存（内容が異なる）。
    g.register_save("a.md", 0x1111, 0);
    g.register_save("a.md", 0x2222, 10);
    EXPECT_EQ(g.pending_count(), 2u);
    // 2 回目の内容に一致するイベントは 2 回目のトークンのみ消費する。
    EXPECT_TRUE(g.consume_if_self("a.md", 0x2222, 20));
    EXPECT_EQ(g.pending_count(), 1u);
    // 残るのは 1 回目（0x1111）。
    EXPECT_TRUE(g.consume_if_self("a.md", 0x1111, 30));
    EXPECT_EQ(g.pending_count(), 0u);
}

TEST(SelfSaveGuardTest, GcDropsStaleTokensThatNeverMatched)
{
    SelfSaveGuard g(100);
    g.register_save("a.md", 0xABCD, 0);
    // 窓(100ms)を超えて外部が即上書きし、一致が永遠に来ないトークンを GC で破棄する。
    g.gc(1000);
    EXPECT_EQ(g.pending_count(), 0u);
}

TEST(SelfSaveGuardTest, UnknownPathNotSuppressed)
{
    SelfSaveGuard g(5000);
    EXPECT_FALSE(g.consume_if_self("never_saved.md", 0xABCD, 0));
}

} // namespace
