#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace telemetryhub::concurrency {

enum class QueueOperationStatus
{
    success,
    full,
    closed,
    cancelled
};

template <typename T>
struct QueuePopResult
{
    QueueOperationStatus status;
    std::optional<T> value;
};

template <typename T>
class BoundedBlockingQueue
{
public:
    // The queue does not own participating threads. All threads performing
    // queue operations must stop before the queue object is destroyed.
    explicit BoundedBlockingQueue(const std::size_t capacity)
        : capacity_{capacity}
    {
        if (capacity == 0U)
        {
            throw std::invalid_argument{"BoundedBlockingQueue capacity must be positive"};
        }
    }

    BoundedBlockingQueue(const BoundedBlockingQueue&) = delete;
    BoundedBlockingQueue& operator=(const BoundedBlockingQueue&) = delete;
    BoundedBlockingQueue(BoundedBlockingQueue&&) = delete;
    BoundedBlockingQueue& operator=(BoundedBlockingQueue&&) = delete;

    [[nodiscard]] QueueOperationStatus push(T value, const std::stop_token stop_token = {})
    {
        std::unique_lock lock{mutex_};
        const bool can_push = not_full_.wait(lock, stop_token, [this] {
            return closed_ || queue_.size() < capacity_;
        });

        if (!can_push)
        {
            return QueueOperationStatus::cancelled;
        }

        if (closed_)
        {
            return QueueOperationStatus::closed;
        }

        queue_.push_back(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
        return QueueOperationStatus::success;
    }

    // Attempts one admission without waiting for queue capacity.
    [[nodiscard]] QueueOperationStatus try_push(T value)
    {
        std::unique_lock lock{mutex_};
        if (closed_)
        {
            return QueueOperationStatus::closed;
        }
        if (queue_.size() >= capacity_)
        {
            return QueueOperationStatus::full;
        }

        queue_.push_back(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
        return QueueOperationStatus::success;
    }

    [[nodiscard]] QueuePopResult<T> pop(const std::stop_token stop_token = {})
    {
        std::unique_lock lock{mutex_};
        const bool can_pop = not_empty_.wait(lock, stop_token, [this] {
            return closed_ || !queue_.empty();
        });

        if (!can_pop)
        {
            return {QueueOperationStatus::cancelled, std::nullopt};
        }

        if (queue_.empty())
        {
            return {QueueOperationStatus::closed, std::nullopt};
        }

        std::optional<T> value{std::move(queue_.front())};
        queue_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return {QueueOperationStatus::success, std::move(value)};
    }

    void close() noexcept
    {
        {
            std::lock_guard lock{mutex_};
            closed_ = true;
        }

        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] bool is_closed() const
    {
        std::lock_guard lock{mutex_};
        return closed_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return capacity_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable_any not_empty_;
    std::condition_variable_any not_full_;
    std::deque<T> queue_;
    bool closed_{false};
};

} // namespace telemetryhub::concurrency
