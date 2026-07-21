#include <telemetryhub/concurrency/worker_pool.hpp>

#include <stdexcept>

namespace telemetryhub::concurrency {

WorkerPool::WorkerPool(const std::size_t worker_count, const std::size_t queue_capacity)
    : tasks_{queue_capacity}
{
    if (worker_count == 0U)
    {
        throw std::invalid_argument{"WorkerPool worker count must be positive"};
    }

    try
    {
        workers_.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index)
        {
            workers_.emplace_back([this] {
                run_worker();
            });
        }
    }
    catch (...)
    {
        tasks_.close();
        for (auto& worker : workers_)
        {
            worker.join();
        }
        throw;
    }
}

WorkerPool::~WorkerPool()
{
    shutdown();
}

void WorkerPool::shutdown() noexcept
{
    std::lock_guard lock{shutdown_mutex_};
    if (stopped_)
    {
        return;
    }

    tasks_.close();
    for (auto& worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
    stopped_ = true;
}

void WorkerPool::run_worker() noexcept
{
    try
    {
        while (true)
        {
            auto result = tasks_.pop();
            if (result.status == QueueOperationStatus::closed)
            {
                return;
            }

            if (result.status == QueueOperationStatus::success && result.value.has_value())
            {
                (*result.value)();
            }
        }
    }
    catch (...)
    {
        // No exception may escape a worker-thread entry function.
    }
}

} // namespace telemetryhub::concurrency
