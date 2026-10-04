#include <telemetryhub/concurrency/bounded_blocking_queue.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using telemetryhub::concurrency::BoundedBlockingQueue;
using telemetryhub::concurrency::QueueOperationStatus;
using namespace std::chrono_literals;

// Only the dedicated blocking tests observe noncompletion: the public API has no wait-entry hook.
constexpr auto blocked_probe = 20ms;
constexpr auto completion_timeout = 2s;

TEST(BoundedBlockingQueue, ConstructsWithPositiveCapacity)
{
    const BoundedBlockingQueue<int> queue{3U};

    EXPECT_EQ(queue.capacity(), 3U);
    EXPECT_FALSE(queue.is_closed());
}

TEST(BoundedBlockingQueue, RejectsZeroCapacity)
{
    EXPECT_THROW(BoundedBlockingQueue<int>{0U}, std::invalid_argument);
}

TEST(BoundedBlockingQueue, PreservesFifoOrder)
{
    BoundedBlockingQueue<int> queue{3U};

    ASSERT_EQ(queue.push(10), QueueOperationStatus::success);
    ASSERT_EQ(queue.push(20), QueueOperationStatus::success);
    ASSERT_EQ(queue.push(30), QueueOperationStatus::success);

    for (const int expected : {10, 20, 30})
    {
        auto result = queue.pop();
        ASSERT_EQ(result.status, QueueOperationStatus::success);
        ASSERT_TRUE(result.value.has_value());
        EXPECT_EQ(*result.value, expected);
    }
}

TEST(BoundedBlockingQueue, SupportsMoveOnlyValues)
{
    BoundedBlockingQueue<std::unique_ptr<int>> queue{1U};

    ASSERT_EQ(queue.push(std::make_unique<int>(42)), QueueOperationStatus::success);
    auto result = queue.pop();

    ASSERT_EQ(result.status, QueueOperationStatus::success);
    ASSERT_TRUE(result.value.has_value());
    ASSERT_NE(*result.value, nullptr);
    EXPECT_EQ(**result.value, 42);
}

TEST(BoundedBlockingQueue, ProducerBlocksWhileFull)
{
    BoundedBlockingQueue<int> queue{1U};
    ASSERT_EQ(queue.push(1), QueueOperationStatus::success);

    std::promise<QueueOperationStatus> completion;
    auto completed = completion.get_future();
    std::latch started{1};
    std::jthread producer{[&queue, &completion, &started] {
        started.count_down();
        completion.set_value(queue.push(2));
    }};

    started.wait();
    EXPECT_EQ(completed.wait_for(blocked_probe), std::future_status::timeout);
    auto first = queue.pop();
    ASSERT_EQ(first.status, QueueOperationStatus::success);
    ASSERT_EQ(completed.wait_for(completion_timeout), std::future_status::ready);
    EXPECT_EQ(completed.get(), QueueOperationStatus::success);
}

TEST(BoundedBlockingQueue, ConsumerBlocksWhileEmpty)
{
    BoundedBlockingQueue<int> queue{1U};
    std::promise<QueueOperationStatus> completion;
    auto completed = completion.get_future();
    std::latch started{1};
    std::jthread consumer{[&queue, &completion, &started] {
        started.count_down();
        completion.set_value(queue.pop().status);
    }};

    started.wait();
    EXPECT_EQ(completed.wait_for(blocked_probe), std::future_status::timeout);
    queue.close();
    ASSERT_EQ(completed.wait_for(completion_timeout), std::future_status::ready);
    EXPECT_EQ(completed.get(), QueueOperationStatus::closed);
}

TEST(BoundedBlockingQueue, PushWakesWaitingConsumer)
{
    BoundedBlockingQueue<int> queue{1U};
    std::promise<telemetryhub::concurrency::QueuePopResult<int>> completion;
    auto completed = completion.get_future();
    std::latch started{1};
    std::jthread consumer{[&queue, &completion, &started] {
        started.count_down();
        completion.set_value(queue.pop());
    }};

    started.wait();
    ASSERT_EQ(queue.push(7), QueueOperationStatus::success);
    ASSERT_EQ(completed.wait_for(completion_timeout), std::future_status::ready);
    auto result = completed.get();
    ASSERT_EQ(result.status, QueueOperationStatus::success);
    ASSERT_TRUE(result.value.has_value());
    EXPECT_EQ(*result.value, 7);
}

TEST(BoundedBlockingQueue, PopWakesWaitingProducer)
{
    BoundedBlockingQueue<int> queue{1U};
    ASSERT_EQ(queue.push(1), QueueOperationStatus::success);

    std::promise<QueueOperationStatus> completion;
    auto completed = completion.get_future();
    std::latch started{1};
    std::jthread producer{[&queue, &completion, &started] {
        started.count_down();
        completion.set_value(queue.push(2));
    }};

    started.wait();
    ASSERT_EQ(queue.pop().status, QueueOperationStatus::success);
    ASSERT_EQ(completed.wait_for(completion_timeout), std::future_status::ready);
    EXPECT_EQ(completed.get(), QueueOperationStatus::success);

    auto second = queue.pop();
    ASSERT_EQ(second.status, QueueOperationStatus::success);
    ASSERT_TRUE(second.value.has_value());
    EXPECT_EQ(*second.value, 2);
}

TEST(BoundedBlockingQueue, CancelsWaitingProducerWithoutAddingValue)
{
    BoundedBlockingQueue<int> queue{1U};
    ASSERT_EQ(queue.push(1), QueueOperationStatus::success);

    std::promise<QueueOperationStatus> completion;
    auto completed = completion.get_future();
    std::latch started{1};
    std::jthread producer{[&queue, &completion, &started](const std::stop_token stop_token) {
        started.count_down();
        completion.set_value(queue.push(2, stop_token));
    }};

    started.wait();
    producer.request_stop();
    ASSERT_EQ(completed.wait_for(completion_timeout), std::future_status::ready);
    EXPECT_EQ(completed.get(), QueueOperationStatus::cancelled);

    auto remaining = queue.pop();
    ASSERT_EQ(remaining.status, QueueOperationStatus::success);
    ASSERT_TRUE(remaining.value.has_value());
    EXPECT_EQ(*remaining.value, 1);
}

TEST(BoundedBlockingQueue, CancelsWaitingConsumer)
{
    BoundedBlockingQueue<int> queue{1U};
    std::promise<QueueOperationStatus> completion;
    auto completed = completion.get_future();
    std::latch started{1};
    std::jthread consumer{[&queue, &completion, &started](const std::stop_token stop_token) {
        started.count_down();
        completion.set_value(queue.pop(stop_token).status);
    }};

    started.wait();
    consumer.request_stop();
    ASSERT_EQ(completed.wait_for(completion_timeout), std::future_status::ready);
    EXPECT_EQ(completed.get(), QueueOperationStatus::cancelled);
}

TEST(BoundedBlockingQueue, ClosingEmptyQueueMakesPopReportClosed)
{
    BoundedBlockingQueue<int> queue{1U};

    queue.close();
    auto result = queue.pop();

    EXPECT_TRUE(queue.is_closed());
    EXPECT_EQ(result.status, QueueOperationStatus::closed);
    EXPECT_FALSE(result.value.has_value());
}

TEST(BoundedBlockingQueue, ClosingNonEmptyQueueAllowsDrainBeforeClosed)
{
    BoundedBlockingQueue<int> queue{2U};
    ASSERT_EQ(queue.push(1), QueueOperationStatus::success);
    ASSERT_EQ(queue.push(2), QueueOperationStatus::success);

    queue.close();

    auto first = queue.pop();
    auto second = queue.pop();
    auto empty = queue.pop();
    ASSERT_EQ(first.status, QueueOperationStatus::success);
    ASSERT_EQ(second.status, QueueOperationStatus::success);
    ASSERT_TRUE(first.value.has_value());
    ASSERT_TRUE(second.value.has_value());
    EXPECT_EQ(*first.value, 1);
    EXPECT_EQ(*second.value, 2);
    EXPECT_EQ(empty.status, QueueOperationStatus::closed);
    EXPECT_FALSE(empty.value.has_value());
}

TEST(BoundedBlockingQueue, RejectsPushAfterClose)
{
    BoundedBlockingQueue<int> queue{1U};
    queue.close();

    EXPECT_EQ(queue.push(1), QueueOperationStatus::closed);
}

TEST(BoundedBlockingQueue, CloseWakesAllBlockedWaiters)
{
    constexpr std::size_t waiter_count = 4U;
    BoundedBlockingQueue<int> full_queue{1U};
    BoundedBlockingQueue<int> empty_queue{1U};
    ASSERT_EQ(full_queue.push(0), QueueOperationStatus::success);

    std::array<std::promise<QueueOperationStatus>, waiter_count> producer_promises;
    std::array<std::promise<QueueOperationStatus>, waiter_count> consumer_promises;
    std::array<std::future<QueueOperationStatus>, waiter_count> producer_futures;
    std::array<std::future<QueueOperationStatus>, waiter_count> consumer_futures;
    for (std::size_t index = 0; index < waiter_count; ++index)
    {
        producer_futures[index] = producer_promises[index].get_future();
        consumer_futures[index] = consumer_promises[index].get_future();
    }

    std::latch started{static_cast<std::ptrdiff_t>(waiter_count * 2U)};
    std::vector<std::jthread> waiters;
    waiters.reserve(waiter_count * 2U);
    for (std::size_t index = 0; index < waiter_count; ++index)
    {
        waiters.emplace_back([&full_queue, &started, &promise = producer_promises[index]] {
            started.count_down();
            promise.set_value(full_queue.push(1));
        });
        waiters.emplace_back([&empty_queue, &started, &promise = consumer_promises[index]] {
            started.count_down();
            promise.set_value(empty_queue.pop().status);
        });
    }

    started.wait();

    full_queue.close();
    empty_queue.close();

    for (std::size_t index = 0; index < waiter_count; ++index)
    {
        ASSERT_EQ(producer_futures[index].wait_for(completion_timeout), std::future_status::ready);
        ASSERT_EQ(consumer_futures[index].wait_for(completion_timeout), std::future_status::ready);
        EXPECT_EQ(producer_futures[index].get(), QueueOperationStatus::closed);
        EXPECT_EQ(consumer_futures[index].get(), QueueOperationStatus::closed);
    }
}

TEST(BoundedBlockingQueue, SupportsMultipleProducersAndConsumers)
{
    constexpr int producer_count = 4;
    constexpr int consumer_count = 4;
    constexpr int values_per_producer = 100;
    constexpr int total_values = producer_count * values_per_producer;

    BoundedBlockingQueue<int> queue{17U};
    std::latch start{producer_count + consumer_count};
    std::atomic<int> operation_failures{0};
    std::mutex consumed_mutex;
    std::vector<int> consumed;
    consumed.reserve(total_values);

    std::vector<std::jthread> consumers;
    consumers.reserve(consumer_count);
    for (int consumer_index = 0; consumer_index < consumer_count; ++consumer_index)
    {
        consumers.emplace_back([&] {
            start.arrive_and_wait();
            while (true)
            {
                auto result = queue.pop();
                if (result.status == QueueOperationStatus::closed)
                {
                    return;
                }
                if (result.status != QueueOperationStatus::success || !result.value.has_value())
                {
                    ++operation_failures;
                    return;
                }

                std::lock_guard lock{consumed_mutex};
                consumed.push_back(*result.value);
            }
        });
    }

    std::vector<std::jthread> producers;
    producers.reserve(producer_count);
    for (int producer_index = 0; producer_index < producer_count; ++producer_index)
    {
        producers.emplace_back([&, producer_index] {
            start.arrive_and_wait();
            const int first_value = producer_index * values_per_producer;
            for (int offset = 0; offset < values_per_producer; ++offset)
            {
                if (queue.push(first_value + offset) != QueueOperationStatus::success)
                {
                    ++operation_failures;
                    return;
                }
            }
        });
    }

    for (auto& producer : producers)
    {
        producer.join();
    }
    queue.close();
    for (auto& consumer : consumers)
    {
        consumer.join();
    }

    EXPECT_EQ(operation_failures.load(), 0);
    ASSERT_EQ(consumed.size(), static_cast<std::size_t>(total_values));
    std::ranges::sort(consumed);
    for (int expected = 0; expected < total_values; ++expected)
    {
        EXPECT_EQ(consumed[static_cast<std::size_t>(expected)], expected);
    }
}

TEST(BoundedBlockingQueue, CloseIsIdempotent)
{
    BoundedBlockingQueue<int> queue{1U};

    queue.close();
    queue.close();
    queue.close();

    EXPECT_TRUE(queue.is_closed());
    EXPECT_EQ(queue.push(1), QueueOperationStatus::closed);
    EXPECT_EQ(queue.pop().status, QueueOperationStatus::closed);
}

TEST(BoundedBlockingQueue, IsSafelyDestroyedAfterParticipatingThreadsStop)
{
    {
        BoundedBlockingQueue<int> queue{1U};
        std::jthread producer{[&queue] {
            static_cast<void>(queue.push(1));
        }};
        producer.join();

        std::jthread consumer{[&queue] {
            static_cast<void>(queue.pop());
        }};
        consumer.join();
        queue.close();
    }

    SUCCEED();
}

} // namespace
