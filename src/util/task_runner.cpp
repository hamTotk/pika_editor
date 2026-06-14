#include "util/task_runner.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace pika::util
{

struct TaskRunner::Impl
{
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> jobs;
    std::mutex mtx;
    std::condition_variable cv;
    bool stopping = false;

    void worker_loop()
    {
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (stopping && jobs.empty())
                {
                    return;
                }
                job = std::move(jobs.front());
                jobs.pop();
            }
            // タスク本体の例外でワーカーを落とさない（design.md
            // 12章「未捕捉例外はアプリを落とさない」）。 packaged_task は例外を future
            // へ格納するため、ここに漏れるのは投入側ラムダの異常のみ。
            try
            {
                job();
            }
            catch (...)
            {
                // 握りつぶしてプールを生かす（個別タスクの失敗はワーカー停止に波及させない）。
            }
        }
    }
};

namespace
{

std::size_t clamp_workers(std::size_t requested)
{
    constexpr std::size_t kMin = 2;
    constexpr std::size_t kMax = 4;
    std::size_t n = requested;
    if (n == 0)
    {
        const unsigned hw = std::thread::hardware_concurrency();
        n = hw == 0 ? kMin : hw;
    }
    return std::clamp<std::size_t>(n, kMin, kMax);
}

} // namespace

TaskRunner::TaskRunner(std::size_t worker_count) : impl_(new Impl)
{
    const std::size_t n = clamp_workers(worker_count);
    impl_->workers.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        impl_->workers.emplace_back([this] { impl_->worker_loop(); });
    }
}

TaskRunner::~TaskRunner()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->stopping = true;
    }
    impl_->cv.notify_all();
    for (auto& t : impl_->workers)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    delete impl_;
}

void TaskRunner::enqueue(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (impl_->stopping)
        {
            return;
        }
        impl_->jobs.push(std::move(job));
    }
    impl_->cv.notify_one();
}

std::size_t TaskRunner::worker_count() const noexcept
{
    return impl_->workers.size();
}

} // namespace pika::util
