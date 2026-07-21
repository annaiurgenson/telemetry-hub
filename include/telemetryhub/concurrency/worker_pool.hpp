#pragma once

#include <telemetryhub/concurrency/bounded_blocking_queue.hpp>

#include <concepts>
#include <cstddef>
#include <future>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace telemetryhub::concurrency {

enum class SubmitStatus
{
    accepted,
    closed,
    cancelled
};

template <typename R>
struct SubmitResult
{
    SubmitStatus status;
    std::optional<std::future<R>> future;
};

class WorkerPool
{
public:
    // Shutdown and destruction must be initiated outside the pool's workers.
    WorkerPool(std::size_t worker_count, std::size_t queue_capacity);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) = delete;
    WorkerPool& operator=(WorkerPool&&) = delete;

    template <typename F>
        requires std::invocable<std::decay_t<F>&>
    [[nodiscard]] auto submit(F&& callable, const std::stop_token stop_token = {})
        -> SubmitResult<std::invoke_result_t<std::decay_t<F>&>>;

    void shutdown() noexcept;

private:
    using Task = std::packaged_task<void()>;

    void run_worker() noexcept;

    BoundedBlockingQueue<Task> tasks_;
    std::vector<std::jthread> workers_;
    std::mutex shutdown_mutex_;
    bool stopped_{false};
};

template <typename F>
    requires std::invocable<std::decay_t<F>&>
auto WorkerPool::submit(F&& callable, const std::stop_token stop_token)
    -> SubmitResult<std::invoke_result_t<std::decay_t<F>&>>
{
    using Result = std::invoke_result_t<std::decay_t<F>&>;

    std::packaged_task<Result()> typed_task{std::forward<F>(callable)};
    auto future = typed_task.get_future();
    Task task{[typed_task = std::move(typed_task)]() mutable {
        typed_task();
    }};

    const QueueOperationStatus queue_status = tasks_.push(std::move(task), stop_token);
    switch (queue_status)
    {
    case QueueOperationStatus::success:
        return {SubmitStatus::accepted, std::move(future)};
    case QueueOperationStatus::closed:
        return {SubmitStatus::closed, std::nullopt};
    case QueueOperationStatus::cancelled:
        return {SubmitStatus::cancelled, std::nullopt};
    }

    return {SubmitStatus::closed, std::nullopt};
}

} // namespace telemetryhub::concurrency
