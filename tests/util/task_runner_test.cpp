// util/task_runner の検証（sprint 2・should）。
// 2〜4 本のワーカープールにタスクを投入し、future で完了・結果が取得できることを観測する。
#include "util/task_runner.h"

#include <atomic>
#include <future>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace
{

using pika::util::TaskRunner;

TEST(TaskRunnerTest, WorkerCountClampedToTwoToFour)
{
    EXPECT_EQ(TaskRunner(1).worker_count(), 2u); // 下限 2
    EXPECT_EQ(TaskRunner(3).worker_count(), 3u);
    EXPECT_EQ(TaskRunner(9).worker_count(), 4u); // 上限 4
    EXPECT_GE(TaskRunner(0).worker_count(), 2u); // 既定（hw 連動）も範囲内
    EXPECT_LE(TaskRunner(0).worker_count(), 4u);
}

TEST(TaskRunnerTest, SubmitReturnsResultViaFuture)
{
    TaskRunner runner(2);
    auto fut = runner.submit([] { return 6 * 7; });
    EXPECT_EQ(fut.get(), 42);
}

TEST(TaskRunnerTest, RunsManyTasksConcurrently)
{
    TaskRunner runner(4);
    constexpr int kN = 200;
    std::vector<std::future<int>> futs;
    futs.reserve(kN);
    for (int i = 0; i < kN; ++i)
    {
        futs.push_back(runner.submit([i] { return i; }));
    }
    int sum = 0;
    for (auto& f : futs)
    {
        sum += f.get();
    }
    EXPECT_EQ(sum, kN * (kN - 1) / 2);
}

TEST(TaskRunnerTest, TaskExceptionPropagatesToFutureNotPool)
{
    // タスク内の例外は future へ伝播し、プールは生き続ける（後続タスクが回る）。
    TaskRunner runner(2);
    auto bad = runner.submit([]() -> int { throw std::runtime_error("boom"); });
    EXPECT_THROW(bad.get(), std::runtime_error);

    auto good = runner.submit([] { return 1; });
    EXPECT_EQ(good.get(), 1);
}

TEST(TaskRunnerTest, VoidTaskRuns)
{
    TaskRunner runner(2);
    std::atomic<int> counter{0};
    auto fut = runner.submit([&counter] { counter.fetch_add(5); });
    fut.get();
    EXPECT_EQ(counter.load(), 5);
}

} // namespace
