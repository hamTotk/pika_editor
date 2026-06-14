// pika util: ワーカープール（TaskRunner、2〜4 本）。
// design.md 3章 util「スレッドプール」/ 4章「ワーカープール（TaskRunner、2〜4本）」/
// 設計原則2「固まらない」。重い処理（ファイル読み込み・差分・スナップショット等）を UI
// スレッドから外す。
//
// UI を一切知らない（コアは wx 非依存）。タスクは std::function<void()> として投入し、
// 完了は std::future で取得する（結果のUI反映はアプリケーション層がコールバック/キューで行う）。
#pragma once

#include <cstddef>
#include <functional>
#include <future>
#include <utility>

namespace pika::util
{

class TaskRunner
{
  public:
    // worker_count はクランプされる（最小 2・最大 4。design.md 4章「2〜4本」）。
    explicit TaskRunner(std::size_t worker_count = 0);
    ~TaskRunner();

    TaskRunner(const TaskRunner&) = delete;
    TaskRunner& operator=(const TaskRunner&) = delete;

    // タスクを投入し、結果を受け取る future を返す。停止後（デストラクタ進行中等）の投入はジョブが
    // 実行されず破棄され、返る future は broken_promise になる（valid() は true だが .get()/.wait()
    // で std::future_error を投げる）。呼び出し側は停止フェーズの get() を例外捕捉するか、future
    // の状態に 依存しないこと（停止後投入は通常の運用経路では発生しない）。
    template <typename F> auto submit(F&& fn) -> std::future<std::invoke_result_t<F>>
    {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        std::future<R> fut = task->get_future();
        enqueue([task]() { (*task)(); });
        return fut;
    }

    std::size_t worker_count() const noexcept;

  private:
    void enqueue(std::function<void()> job);

    struct Impl;
    Impl* impl_;
};

} // namespace pika::util
