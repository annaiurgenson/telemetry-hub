#include <telemetryhub/concurrency/worker_pool.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace {

using telemetryhub::concurrency::SubmitStatus;
using telemetryhub::concurrency::WorkerPool;
using namespace std::chrono_literals;

// Noncompletion checks remain only for backpressure and synchronous shutdown.
constexpr auto blocked_probe = 20ms;
constexpr auto completion_timeout = 2s;

template <typename R>
std::future<R> take_accepted_future(telemetryhub::concurrency::SubmitResult<R> result)
{
    EXPECT_EQ(result.status, SubmitStatus::accepted);
    EXPECT_TRUE(result.future.has_value());
    if (!result.future.has_value())
    {
        return {};
    }
    return std::move(*result.future);
}

TEST(WorkerPool, RejectsZeroWorkerCount)
{
    EXPECT_THROW(WorkerPool(0U, 1U), std::invalid_argument);
}

TEST(WorkerPool, RejectsZeroQueueCapacity)
{
    EXPECT_THROW(WorkerPool(1U, 0U), std::invalid_argument);
}

TEST(WorkerPool, ExecutesOneTask)
{
    WorkerPool pool{1U, 1U};
    std::promise<void> executed;
    auto execution = executed.get_future();

    auto task = take_accepted_future(pool.submit([&executed] {
        executed.set_value();
    }));

    ASSERT_EQ(execution.wait_for(completion_timeout), std::future_status::ready);
    task.get();
}

TEST(WorkerPool, ReturnsNonVoidResult)
{
    WorkerPool pool{1U, 1U};
    auto result = take_accepted_future(pool.submit([] {
        return 42;
    }));

    EXPECT_EQ(result.get(), 42);
}

TEST(WorkerPool, ExecutesVoidTask)
{
    WorkerPool pool{1U, 1U};
    std::atomic<bool> executed{false};
    auto result = take_accepted_future(pool.submit([&executed] {
        executed = true;
    }));

    result.get();
    EXPECT_TRUE(executed.load());
}

TEST(WorkerPool, AcceptsMoveOnlyCallable)
{
    WorkerPool pool{1U, 1U};
    auto value = std::make_unique<int>(17);
    auto result = take_accepted_future(pool.submit([value = std::move(value)] {
        return *value;
    }));

    EXPECT_EQ(result.get(), 17);
}

TEST(WorkerPool, PropagatesTaskExceptionThroughFuture)
{
    WorkerPool pool{1U, 1U};
    auto result = take_accepted_future(pool.submit([]() -> int {
        throw std::runtime_error{"task failed"};
    }));

    EXPECT_THROW(static_cast<void>(result.get()), std::runtime_error);
}

TEST(WorkerPool, WorkerContinuesAfterTaskThrows)
{
    WorkerPool pool{1U, 2U};
    auto failing = take_accepted_future(pool.submit([] {
        throw std::runtime_error{"task failed"};
    }));
    auto succeeding = take_accepted_future(pool.submit([] {
        return 9;
    }));

    EXPECT_THROW(failing.get(), std::runtime_error);
    EXPECT_EQ(succeeding.get(), 9);
}

TEST(WorkerPool, ExecutesMultipleTasks)
{
    WorkerPool pool{2U, 8U};
    std::atomic<int> executions{0};
    std::vector<std::future<void>> futures;
    for (int index = 0; index < 8; ++index)
    {
        futures.push_back(take_accepted_future(pool.submit([&executions] {
            ++executions;
        })));
    }

    for (auto& future : futures)
    {
        future.get();
    }
    EXPECT_EQ(executions.load(), 8);
}

TEST(WorkerPool, ExecutesTasksInParallelWithMultipleWorkers)
{
    constexpr std::ptrdiff_t worker_count = 3;
    WorkerPool pool{static_cast<std::size_t>(worker_count), static_cast<std::size_t>(worker_count)};
    std::latch entered{worker_count};
    std::promise<void> release;
    const auto release_signal = release.get_future().share();
    std::vector<std::future<void>> futures;

    for (std::ptrdiff_t index = 0; index < worker_count; ++index)
    {
        futures.push_back(take_accepted_future(pool.submit([&entered, release_signal] {
            entered.count_down();
            release_signal.wait();
        })));
    }

    entered.wait();
    release.set_value();
    for (auto& future : futures)
    {
        future.get();
    }
}

TEST(WorkerPool, AppliesBackpressureWhenQueueIsFull)
{
    WorkerPool pool{1U, 1U};
    std::promise<void> release;
    const auto release_signal = release.get_future().share();
    std::promise<void> running;
    auto running_signal = running.get_future();

    auto first = take_accepted_future(pool.submit([&running, release_signal] {
        running.set_value();
        release_signal.wait();
    }));
    ASSERT_EQ(running_signal.wait_for(completion_timeout), std::future_status::ready);
    auto second = take_accepted_future(pool.submit([] {}));

    std::latch submitting{1};
    auto blocked_submission = std::async(std::launch::async, [&pool, &submitting] {
        submitting.count_down();
        return pool.submit([] {});
    });
    submitting.wait();
    EXPECT_EQ(blocked_submission.wait_for(blocked_probe), std::future_status::timeout);

    release.set_value();
    auto third = take_accepted_future(blocked_submission.get());
    first.get();
    second.get();
    third.get();
}

TEST(WorkerPool, CancelsSubmissionBlockedByFullQueue)
{
    WorkerPool pool{1U, 1U};
    std::promise<void> release;
    const auto release_signal = release.get_future().share();
    std::promise<void> running;
    auto running_signal = running.get_future();

    auto first = take_accepted_future(pool.submit([&running, release_signal] {
        running.set_value();
        release_signal.wait();
    }));
    ASSERT_EQ(running_signal.wait_for(completion_timeout), std::future_status::ready);
    auto second = take_accepted_future(pool.submit([] {}));

    std::promise<telemetryhub::concurrency::SubmitResult<void>> completion;
    auto completed = completion.get_future();
    std::latch submitting{1};
    std::jthread producer{[&pool, &completion, &submitting](const std::stop_token stop_token) {
        submitting.count_down();
        completion.set_value(pool.submit([] {}, stop_token));
    }};
    submitting.wait();
    producer.request_stop();

    ASSERT_EQ(completed.wait_for(completion_timeout), std::future_status::ready);
    auto cancelled = completed.get();
    EXPECT_EQ(cancelled.status, SubmitStatus::cancelled);
    EXPECT_FALSE(cancelled.future.has_value());

    release.set_value();
    first.get();
    second.get();
}

TEST(WorkerPool, GracefulShutdownDrainsQueuedTasks)
{
    WorkerPool pool{1U, 3U};
    std::promise<void> release;
    const auto release_signal = release.get_future().share();
    std::promise<void> running;
    auto running_signal = running.get_future();
    std::atomic<int> executions{0};

    auto first = take_accepted_future(pool.submit([&running, release_signal, &executions] {
        running.set_value();
        release_signal.wait();
        ++executions;
    }));
    ASSERT_EQ(running_signal.wait_for(completion_timeout), std::future_status::ready);
    auto second = take_accepted_future(pool.submit([&executions] { ++executions; }));
    auto third = take_accepted_future(pool.submit([&executions] { ++executions; }));

    std::latch stopping{1};
    auto shutdown = std::async(std::launch::async, [&pool, &stopping] {
        stopping.count_down();
        pool.shutdown();
    });
    stopping.wait();
    EXPECT_EQ(shutdown.wait_for(blocked_probe), std::future_status::timeout);
    release.set_value();
    ASSERT_EQ(shutdown.wait_for(completion_timeout), std::future_status::ready);

    first.get();
    second.get();
    third.get();
    EXPECT_EQ(executions.load(), 3);
}

TEST(WorkerPool, ShutdownWakesAndRejectsBlockedSubmission)
{
    WorkerPool pool{1U, 1U};
    std::promise<void> release;
    const auto release_signal = release.get_future().share();
    std::promise<void> running;
    auto running_signal = running.get_future();

    auto first = take_accepted_future(pool.submit([&running, release_signal] {
        running.set_value();
        release_signal.wait();
    }));
    ASSERT_EQ(running_signal.wait_for(completion_timeout), std::future_status::ready);
    auto second = take_accepted_future(pool.submit([] {}));

    std::latch submitting{1};
    auto blocked_submission = std::async(std::launch::async, [&pool, &submitting] {
        submitting.count_down();
        return pool.submit([] {});
    });
    submitting.wait();
    auto shutdown = std::async(std::launch::async, [&pool] {
        pool.shutdown();
    });

    ASSERT_EQ(blocked_submission.wait_for(completion_timeout), std::future_status::ready);
    auto rejected = blocked_submission.get();
    EXPECT_EQ(rejected.status, SubmitStatus::closed);
    EXPECT_FALSE(rejected.future.has_value());

    release.set_value();
    shutdown.get();
    first.get();
    second.get();
}

TEST(WorkerPool, RejectsSubmissionAfterShutdown)
{
    WorkerPool pool{1U, 1U};
    pool.shutdown();

    auto result = pool.submit([] {});
    EXPECT_EQ(result.status, SubmitStatus::closed);
    EXPECT_FALSE(result.future.has_value());
}

TEST(WorkerPool, RepeatedShutdownCallsAreSafe)
{
    WorkerPool pool{1U, 1U};
    pool.shutdown();
    pool.shutdown();
    pool.shutdown();

    EXPECT_EQ(pool.submit([] {}).status, SubmitStatus::closed);
}

TEST(WorkerPool, DestructorPerformsGracefulShutdown)
{
    std::atomic<int> executions{0};
    std::future<void> first;
    std::future<void> second;
    {
        WorkerPool pool{1U, 2U};
        first = take_accepted_future(pool.submit([&executions] { ++executions; }));
        second = take_accepted_future(pool.submit([&executions] { ++executions; }));
    }

    first.get();
    second.get();
    EXPECT_EQ(executions.load(), 2);
}

TEST(WorkerPool, ConcurrentProducersLoseNoAcceptedTasks)
{
    constexpr int producer_count = 4;
    constexpr int tasks_per_producer = 50;
    constexpr int task_count = producer_count * tasks_per_producer;
    WorkerPool pool{4U, 17U};
    std::latch start{producer_count};
    std::atomic<int> executions{0};
    std::atomic<int> submission_failures{0};
    std::vector<std::jthread> producers;

    for (int producer_index = 0; producer_index < producer_count; ++producer_index)
    {
        producers.emplace_back([&] {
            start.arrive_and_wait();
            for (int index = 0; index < tasks_per_producer; ++index)
            {
                auto result = pool.submit([&executions] { ++executions; });
                if (result.status != SubmitStatus::accepted || !result.future.has_value())
                {
                    ++submission_failures;
                }
            }
        });
    }

    for (auto& producer : producers)
    {
        producer.join();
    }
    pool.shutdown();

    EXPECT_EQ(submission_failures.load(), 0);
    EXPECT_EQ(executions.load(), task_count);
}

TEST(WorkerPool, IsCleanlyDestroyedAfterWorkersFinish)
{
    {
        WorkerPool pool{2U, 2U};
        auto first = take_accepted_future(pool.submit([] {}));
        auto second = take_accepted_future(pool.submit([] {}));
        first.get();
        second.get();
        pool.shutdown();
    }

    SUCCEED();
}

} // namespace
