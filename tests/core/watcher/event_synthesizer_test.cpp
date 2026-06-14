// core/watcher イベント合成の検証（sprint3 must「イベント合成」）。
// 連続書き込み（短時間に複数イベント）がデバウンス（100ms目安）で 1 つの FsEvent に合成される
// ことを観測する（design.md 5.2・13章）。純ロジックのため時刻を注入して決定論検証する。
#include "core/watcher/event_synthesizer.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::watcher::EventSynthesizer;
using pika::core::watcher::FsEventKind;
using pika::core::watcher::RawAction;
using pika::core::watcher::RawEvent;

RawEvent raw(RawAction a, const char* path, pika::core::watcher::TimeMs at)
{
    return RawEvent{a, path, at};
}

TEST(EventSynthesizerTest, CoalescesBurstWritesIntoOneEvent)
{
    EventSynthesizer s(100);
    // 0,10,20,30ms に連続書き込み（同一パス）。
    s.push(raw(RawAction::Modified, "a.md", 0));
    s.push(raw(RawAction::Modified, "a.md", 10));
    s.push(raw(RawAction::Modified, "a.md", 20));
    s.push(raw(RawAction::Modified, "a.md", 30));

    // 静穏期間未満では確定しない。
    EXPECT_TRUE(s.flush(100).empty()); // 30+100=130 未満
    EXPECT_TRUE(s.flush(129).empty());

    // 最後の生イベント(30ms)から100ms静穏で 1 件に合成される。
    auto out = s.flush(130);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].path, "a.md");
    EXPECT_EQ(out[0].kind, FsEventKind::Modified);
    EXPECT_EQ(out[0].at, 30u); // 確定時刻＝最後の生イベント時刻

    // 確定後はバッファが空。
    EXPECT_TRUE(s.empty());
}

TEST(EventSynthesizerTest, DistinctPathsAreSeparateEvents)
{
    EventSynthesizer s(100);
    s.push(raw(RawAction::Modified, "a.md", 0));
    s.push(raw(RawAction::Modified, "b.md", 5));

    auto out = s.flush(200);
    ASSERT_EQ(out.size(), 2u);
    // 確定時刻昇順（a=0, b=5）。
    EXPECT_EQ(out[0].path, "a.md");
    EXPECT_EQ(out[1].path, "b.md");
}

TEST(EventSynthesizerTest, NewWriteWithinWindowKeepsPathPending)
{
    EventSynthesizer s(100);
    s.push(raw(RawAction::Modified, "a.md", 0));
    // 90msで再書き込み→静穏起点が90msへ更新される。
    s.push(raw(RawAction::Modified, "a.md", 90));
    EXPECT_TRUE(s.flush(150).empty()); // 90+100=190 未満
    auto out = s.flush(190);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].at, 90u);
}

TEST(EventSynthesizerTest, AddThenModifyCoalescesToCreated)
{
    EventSynthesizer s(100);
    s.push(raw(RawAction::Added, "new.md", 0));
    s.push(raw(RawAction::Modified, "new.md", 10));
    auto out = s.flush(200);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].kind, FsEventKind::Created);
}

TEST(EventSynthesizerTest, RemovedDominatesWhenLast)
{
    EventSynthesizer s(100);
    s.push(raw(RawAction::Added, "tmp.md", 0));
    s.push(raw(RawAction::Modified, "tmp.md", 5));
    s.push(raw(RawAction::Removed, "tmp.md", 10));
    auto out = s.flush(200);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].kind, FsEventKind::Removed);
}

TEST(EventSynthesizerTest, FlushAllIgnoresWindow)
{
    EventSynthesizer s(100);
    s.push(raw(RawAction::Modified, "a.md", 1000));
    // 窓未経過でも flush_all は全て確定する（再同期前ドレイン用）。
    auto out = s.flush_all();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(s.empty());
}

} // namespace
