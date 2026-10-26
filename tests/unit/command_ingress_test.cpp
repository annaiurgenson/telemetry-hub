#include <telemetryhub/domain/telemetry_processor.hpp>
#include <telemetryhub/domain/telemetry_store.hpp>
#include <telemetryhub/protocol/message_codec.hpp>
#include <telemetryhub/protocol/message_framer.hpp>
#include <telemetryhub/server/command_ingress.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using telemetryhub::domain::TelemetryProcessor;
using telemetryhub::domain::TelemetryStore;
using telemetryhub::network::ConnectionId;
using telemetryhub::protocol::Response;
using telemetryhub::server::CommandAdmission;
using telemetryhub::server::CommandEnvelope;
using telemetryhub::server::CommandIngress;
using telemetryhub::server::ProcessingCommand;

[[nodiscard]] CommandEnvelope make_envelope(
    const std::uint64_t connection_id,
    const std::uint64_t request_id,
    const double value = 21.5)
{
    return CommandEnvelope{
        ConnectionId{connection_id}, request_id, "device-snapshot",
        ProcessingCommand{telemetryhub::protocol::SubmitTelemetry{"temperature", value}},
        telemetryhub::domain::ReceivedAt{std::chrono::milliseconds{static_cast<std::int64_t>(request_id)}},
        0U};
}

[[nodiscard]] Response decode_response(const std::vector<std::byte>& frame)
{
    const std::size_t payload_size = frame.size() - telemetryhub::protocol::frame_header_size_bytes;
    auto result = telemetryhub::protocol::parse_response_payload(
        std::span<const std::byte>{frame}.subspan(telemetryhub::protocol::frame_header_size_bytes, payload_size));
    const auto* response = std::get_if<Response>(&result);
    if (response == nullptr)
    {
        throw std::runtime_error{"consumer produced an invalid response frame"};
    }
    return *response;
}

TEST(CommandIngress, ProcessesOwnedEnvelopeAndReturnsCorrelatedAcknowledgment)
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    telemetryhub::concurrency::WorkerPool worker_pool{4U, 1024U};
    CommandIngress ingress{processor, worker_pool, 2U};
    std::promise<std::pair<ConnectionId, std::vector<std::byte>>> completed;
    auto result = completed.get_future();
    ingress.start([&completed](const ConnectionId connection_id, std::vector<std::byte> bytes) {
        completed.set_value({connection_id, std::move(bytes)});
        return telemetryhub::network::EnqueueStatus::accepted;
    });

    EXPECT_EQ(ingress.submit(make_envelope(71U, 9001U)), CommandAdmission::accepted);
    ASSERT_EQ(result.wait_for(2s), std::future_status::ready);
    auto [connection_id, frame] = result.get();
    EXPECT_EQ(connection_id, ConnectionId{71U});
    const auto response = decode_response(frame);
    EXPECT_EQ(response.request_id, 9001U);
    EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::TelemetryAccepted>(response.body));
    const auto statistics = store.statistics_for("device-snapshot", "temperature");
    ASSERT_TRUE(statistics.has_value());
    EXPECT_EQ(statistics->sample_count, 1U);
    ingress.shutdown();
    worker_pool.shutdown();
}

TEST(CommandIngress, BoundedQueueReportsFullAndShutdownDrainsAcceptedCommands)
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    telemetryhub::concurrency::WorkerPool worker_pool{1U, 1U};
    CommandIngress ingress{processor, worker_pool, 1U};
    std::latch first_response_entered{1};
    std::latch release_first_response{1};
    std::atomic<unsigned int> response_count{0U};
    std::atomic<bool> hold_first{true};
    ingress.start([&](const ConnectionId, std::vector<std::byte>) {
        if (hold_first.exchange(false))
        {
            first_response_entered.count_down();
            release_first_response.wait();
        }
        response_count.fetch_add(1U);
        return telemetryhub::network::EnqueueStatus::accepted;
    });

    EXPECT_EQ(ingress.submit(make_envelope(1U, 1U, 1.0)), CommandAdmission::accepted);
    first_response_entered.wait();
    std::vector<double> admitted_values{1.0};
    bool observed_full = false;
    for (std::uint64_t request_id = 2U; request_id < 10U; ++request_id)
    {
        const double value = request_id == 2U ? 2.0 : static_cast<double>(request_id * 100U);
        const auto admission = ingress.submit(make_envelope(1U, request_id, value));
        if (admission == CommandAdmission::full)
        {
            observed_full = true;
            break;
        }
        ASSERT_EQ(admission, CommandAdmission::accepted);
        admitted_values.push_back(value);
    }
    ASSERT_TRUE(observed_full);

    auto shutdown = std::async(std::launch::async, [&] { ingress.shutdown(); });
    release_first_response.count_down();
    ASSERT_EQ(shutdown.wait_for(3s), std::future_status::ready);
    shutdown.get();
    worker_pool.shutdown();

    EXPECT_EQ(response_count.load(), admitted_values.size());
    EXPECT_EQ(ingress.submit(make_envelope(1U, 10U)), CommandAdmission::closed);
    const auto statistics = store.statistics_for("device-snapshot", "temperature");
    ASSERT_TRUE(statistics.has_value());
    EXPECT_EQ(statistics->sample_count, admitted_values.size());
    EXPECT_DOUBLE_EQ(statistics->last_value, admitted_values.back());
}
TEST(CommandIngress, ConcurrentShutdownCallsAreIdempotentAndDrainOnce)
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    telemetryhub::concurrency::WorkerPool worker_pool{4U, 1024U};
    CommandIngress ingress{processor, worker_pool, 2U};
    std::latch response_entered{1};
    std::latch release_response{1};
    std::atomic<unsigned int> response_count{0U};
    ingress.start([&](const ConnectionId, std::vector<std::byte>) {
        response_entered.count_down();
        release_response.wait();
        response_count.fetch_add(1U);
        return telemetryhub::network::EnqueueStatus::accepted;
    });

    EXPECT_EQ(ingress.submit(make_envelope(3U, 1U)), CommandAdmission::accepted);
    response_entered.wait();
    std::barrier shutdown_together{3};
    std::jthread first_shutdown{[&] {
        shutdown_together.arrive_and_wait();
        ingress.shutdown();
    }};
    std::jthread second_shutdown{[&] {
        shutdown_together.arrive_and_wait();
        ingress.shutdown();
    }};
    shutdown_together.arrive_and_wait();
    release_response.count_down();
    first_shutdown.join();
    second_shutdown.join();
    worker_pool.shutdown();

    EXPECT_EQ(response_count.load(), 1U);
    EXPECT_EQ(ingress.submit(make_envelope(3U, 2U)), CommandAdmission::closed);
    EXPECT_EQ(store.statistics_for("device-snapshot", "temperature")->sample_count, 1U);
}

TEST(CommandIngress, ConcurrentProducerStopsAtClosedAdmission)
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    telemetryhub::concurrency::WorkerPool worker_pool{1U, 1U};
    CommandIngress ingress{processor, worker_pool, 8U};
    std::latch first_response_entered{1};
    std::latch release_first_response{1};
    std::latch producer_saw_full{1};
    std::latch producer_saw_closed{1};
    std::atomic<bool> hold_first_response{true};
    std::atomic<unsigned int> consumed{0U};
    ingress.start([&](const ConnectionId, std::vector<std::byte>) {
        if (hold_first_response.exchange(false))
        {
            first_response_entered.count_down();
            release_first_response.wait();
        }
        consumed.fetch_add(1U);
        return telemetryhub::network::EnqueueStatus::accepted;
    });

    std::atomic<unsigned int> accepted{0U};
    std::atomic<bool> reported_full{false};
    std::jthread producer_thread{[&] {
        std::uint64_t request_id = 1U;
        while (request_id <= 1'000U)
        {
            const auto status = ingress.submit(make_envelope(2U, request_id));
            if (status == CommandAdmission::accepted)
            {
                accepted.fetch_add(1U);
                ++request_id;
            }
            else if (status == CommandAdmission::closed)
            {
                producer_saw_closed.count_down();
                break;
            }
            else if (status == CommandAdmission::full)
            {
                if (!reported_full.exchange(true))
                {
                    producer_saw_full.count_down();
                }
                std::this_thread::yield();
            }
            else
            {
                ADD_FAILURE() << "unexpected sequence exhaustion";
                break;
            }
        }
    }};

    first_response_entered.wait();
    producer_saw_full.wait();
    auto shutdown = std::async(std::launch::async, [&] { ingress.shutdown(); });
    producer_saw_closed.wait();
    release_first_response.count_down();
    ASSERT_EQ(shutdown.wait_for(3s), std::future_status::ready);
    shutdown.get();
    producer_thread.join();

    EXPECT_GT(accepted.load(), 1U);
    worker_pool.shutdown();
    EXPECT_EQ(consumed.load(), accepted.load());
    EXPECT_EQ(store.statistics_for("device-snapshot", "temperature")->sample_count, accepted.load());
}

TEST(CommandIngress, SinkExceptionIsNotRetriedAndLaterCommandsStillProcess)
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    telemetryhub::concurrency::WorkerPool worker_pool{1U, 4U};
    CommandIngress ingress{processor, worker_pool, 4U};
    std::latch second_response_entered{1};
    std::atomic<unsigned int> invocations{0U};
    std::promise<Response> second_response;
    auto result = second_response.get_future();
    ingress.start([&](const ConnectionId, std::vector<std::byte> bytes) {
        const unsigned int invocation = invocations.fetch_add(1U);
        if (invocation == 0U)
        {
            throw std::runtime_error{"ambiguous sink failure"};
        }
        second_response.set_value(decode_response(bytes));
        second_response_entered.count_down();
        return telemetryhub::network::EnqueueStatus::accepted;
    });

    EXPECT_EQ(ingress.submit(make_envelope(4U, 10U, 1.0)), CommandAdmission::accepted);
    EXPECT_EQ(ingress.submit(make_envelope(4U, 11U, 2.0)), CommandAdmission::accepted);
    second_response_entered.wait();
    ASSERT_EQ(result.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(result.get().request_id, 11U);
    ingress.shutdown();
    worker_pool.shutdown();
    EXPECT_EQ(invocations.load(), 2U);
    const auto statistics = store.statistics_for("device-snapshot", "temperature");
    ASSERT_TRUE(statistics.has_value());
    EXPECT_EQ(statistics->sample_count, 2U);
    EXPECT_DOUBLE_EQ(statistics->last_value, 2.0);
}

TEST(CommandIngress, ConcurrentSubmissionsUseOneProcessorAndDrain)
{
    constexpr std::size_t producer_count = 6U;
    constexpr std::size_t submissions_per_producer = 100U;
    TelemetryStore store;
    TelemetryProcessor processor{store};
    telemetryhub::concurrency::WorkerPool worker_pool{4U, 600U};
    CommandIngress ingress{processor, worker_pool, producer_count * submissions_per_producer};
    std::atomic<std::size_t> responses{0U};
    ingress.start([&responses](const ConnectionId, std::vector<std::byte>) {
        responses.fetch_add(1U);
        return telemetryhub::network::EnqueueStatus::accepted;
    });

    std::latch start{static_cast<std::ptrdiff_t>(producer_count + 1U)};
    std::atomic<unsigned int> rejected{0U};
    std::vector<std::jthread> producers;
    producers.reserve(producer_count);
    for (std::size_t producer = 0; producer < producer_count; ++producer)
    {
        producers.emplace_back([&, producer] {
            start.count_down();
            start.wait();
            for (std::size_t index = 0; index < submissions_per_producer; ++index)
            {
                const auto request_id = static_cast<std::uint64_t>(producer * submissions_per_producer + index + 1U);
                const auto status = ingress.submit(make_envelope(static_cast<std::uint64_t>(producer + 1U), request_id, static_cast<double>(producer)));
                if (status != CommandAdmission::accepted)
                {
                    rejected.fetch_add(1U);
                }
            }
        });
    }
    start.count_down();
    for (auto& producer : producers)
    {
        producer.join();
    }
    ingress.shutdown();
    worker_pool.shutdown();

    EXPECT_EQ(rejected.load(), 0U);
    EXPECT_EQ(responses.load(), producer_count * submissions_per_producer);
    const auto statistics = store.statistics_for("device-snapshot", "temperature");
    ASSERT_TRUE(statistics.has_value());
    EXPECT_EQ(statistics->sample_count, producer_count * submissions_per_producer);
    EXPECT_DOUBLE_EQ(statistics->minimum, 0.0);
    EXPECT_DOUBLE_EQ(statistics->maximum, static_cast<double>(producer_count - 1U));
}
TEST(CommandIngress, WorkersRunConcurrentlyAndResponsesRetainRequestIds)
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    telemetryhub::concurrency::WorkerPool worker_pool{2U, 2U};
    CommandIngress ingress{processor, worker_pool, 4U};
    std::latch both_workers_entered_sink{2};
    std::latch release_first_response{1};
    std::latch first_response_finished{1};
    std::promise<Response> second_response;
    auto second = second_response.get_future();
    ingress.start([&](const ConnectionId, std::vector<std::byte> bytes) {
        const Response response = decode_response(bytes);
        if (response.request_id == 10U)
        {
            both_workers_entered_sink.count_down();
            release_first_response.wait();
            first_response_finished.count_down();
        }
        else
        {
            second_response.set_value(response);
            both_workers_entered_sink.count_down();
        }
        return telemetryhub::network::EnqueueStatus::accepted;
    });

    EXPECT_EQ(ingress.submit(make_envelope(8U, 10U, 1.0)), CommandAdmission::accepted);
    EXPECT_EQ(ingress.submit(make_envelope(8U, 11U, 2.0)), CommandAdmission::accepted);
    both_workers_entered_sink.wait();
    ASSERT_EQ(second.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(second.get().request_id, 11U);
    release_first_response.count_down();
    first_response_finished.wait();
    ingress.shutdown();
    worker_pool.shutdown();

    const auto statistics = store.statistics_for("device-snapshot", "temperature");
    ASSERT_TRUE(statistics.has_value());
    EXPECT_EQ(statistics->sample_count, 2U);
    EXPECT_DOUBLE_EQ(statistics->last_value, 2.0);
}

TEST(CommandIngress, RejectedFullQueueSubmissionDoesNotAlterLaterStatistics)
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    telemetryhub::concurrency::WorkerPool worker_pool{1U, 1U};
    CommandIngress ingress{processor, worker_pool, 1U};
    std::latch first_response_entered{1};
    std::latch release_first_response{1};
    std::atomic<bool> hold_first{true};
    std::atomic<std::size_t> response_count{0U};
    std::atomic<std::size_t> initial_target{0U};
    std::promise<void> initial_responses_completed;
    auto initial_done = initial_responses_completed.get_future();
    std::promise<void> later_response_completed;
    auto later_done = later_response_completed.get_future();
    ingress.start([&](const ConnectionId, std::vector<std::byte> bytes) {
        if (hold_first.exchange(false))
        {
            first_response_entered.count_down();
            release_first_response.wait();
        }
        const auto count = response_count.fetch_add(1U) + 1U;
        if (count == initial_target.load())
        {
            initial_responses_completed.set_value();
        }
        if (decode_response(bytes).request_id == 100U)
        {
            later_response_completed.set_value();
        }
        return telemetryhub::network::EnqueueStatus::accepted;
    });

    EXPECT_EQ(ingress.submit(make_envelope(1U, 1U, 1.0)), CommandAdmission::accepted);
    first_response_entered.wait();
    std::vector<double> admitted_values{1.0};
    bool observed_full = false;
    for (std::uint64_t request_id = 2U; request_id < 10U; ++request_id)
    {
        const double value = request_id == 2U ? 2.0 : static_cast<double>(request_id * 100U);
        const auto admission = ingress.submit(make_envelope(1U, request_id, value));
        if (admission == CommandAdmission::full)
        {
            observed_full = true;
            break;
        }
        ASSERT_EQ(admission, CommandAdmission::accepted);
        admitted_values.push_back(value);
    }
    ASSERT_TRUE(observed_full);
    initial_target.store(admitted_values.size());

    release_first_response.count_down();
    ASSERT_EQ(initial_done.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(ingress.submit(make_envelope(1U, 100U, 4.0)), CommandAdmission::accepted);
    ASSERT_EQ(later_done.wait_for(3s), std::future_status::ready);
    ingress.shutdown();
    worker_pool.shutdown();

    const auto statistics = store.statistics_for("device-snapshot", "temperature");
    ASSERT_TRUE(statistics.has_value());
    EXPECT_EQ(statistics->sample_count, admitted_values.size() + 1U);
    EXPECT_DOUBLE_EQ(statistics->last_value, 4.0);
    EXPECT_LT(statistics->maximum, 500.0);
    EXPECT_EQ(response_count.load(), statistics->sample_count);
}

TEST(CommandIngress, ShutdownDrainsWhenDispatcherIsBlockedByFullWorkerPool)
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    std::latch release_worker{1};
    std::promise<void> worker_started_promise;
    auto worker_started = worker_started_promise.get_future();
    std::atomic<unsigned int> blocker_executions{0U};
    std::atomic<unsigned int> filler_executions{0U};
    std::mutex response_mutex;
    std::vector<std::uint64_t> response_ids;
    std::promise<void> shutdown_started_promise;
    auto shutdown_started = shutdown_started_promise.get_future();
    std::future<void> shutdown;

    telemetryhub::concurrency::WorkerPool worker_pool{1U, 1U};
    CommandIngress ingress{processor, worker_pool, 1U};

    struct ReleaseWorkerOnExit
    {
        ReleaseWorkerOnExit(
            std::latch& gate,
            std::future<void>& shutdown) noexcept
            : gate_{gate}, shutdown_{shutdown}
        {
        }

        ~ReleaseWorkerOnExit() noexcept
        {
            release();
            if (shutdown_.valid())
            {
                shutdown_.wait();
            }
        }

        void release() noexcept
        {
            if (!released_)
            {
                released_ = true;
                gate_.count_down();
            }
        }

        std::latch& gate_;
        std::future<void>& shutdown_;
        bool released_{false};
    } release_worker_on_exit{release_worker, shutdown};

    auto blocker_submission = worker_pool.submit([&] {
        worker_started_promise.set_value();
        release_worker.wait();
        blocker_executions.fetch_add(1U);
    });
    ASSERT_EQ(blocker_submission.status, telemetryhub::concurrency::SubmitStatus::accepted);
    ASSERT_TRUE(blocker_submission.future.has_value());
    ASSERT_EQ(worker_started.wait_for(3s), std::future_status::ready);
    worker_started.get();

    auto filler_submission = worker_pool.submit([&] {
        filler_executions.fetch_add(1U);
    });
    ASSERT_EQ(filler_submission.status, telemetryhub::concurrency::SubmitStatus::accepted);
    ASSERT_TRUE(filler_submission.future.has_value());

    ingress.start([&](const ConnectionId, std::vector<std::byte> bytes) {
        const Response response = decode_response(bytes);
        std::lock_guard lock{response_mutex};
        response_ids.push_back(response.request_id);
        return telemetryhub::network::EnqueueStatus::accepted;
    });

    ASSERT_EQ(ingress.submit(make_envelope(9U, 1U, 1.0)), CommandAdmission::accepted);

    // A successful admission for B proves A has left ingress. The worker is
    // still gated and the pool queue is occupied by the filler, so the single
    // dispatcher cannot finish handing A to the pool. Keep retrying only while
    // ingress reports full; the deadline is a safety bound, not synchronization.
    CommandAdmission command_b_admission = CommandAdmission::full;
    const auto command_b_deadline = std::chrono::steady_clock::now() + 3s;
    while (command_b_admission == CommandAdmission::full &&
           std::chrono::steady_clock::now() < command_b_deadline)
    {
        command_b_admission = ingress.submit(make_envelope(9U, 2U, 2.0));
        if (command_b_admission == CommandAdmission::full)
        {
            std::this_thread::yield();
        }
    }
    ASSERT_EQ(command_b_admission, CommandAdmission::accepted);
    EXPECT_EQ(ingress.submit(make_envelope(9U, 3U, 300.0)), CommandAdmission::full);

    shutdown = std::async(std::launch::async, [&] {
        shutdown_started_promise.set_value();
        ingress.shutdown();
    });
    ASSERT_EQ(shutdown_started.wait_for(3s), std::future_status::ready);

    // Admission remains full until shutdown closes the queue. This observes
    // queue closure without claiming to observe the dispatcher's instruction
    // pointer inside WorkerPool::submit().
    bool admission_closed = false;
    bool unexpected_probe_admission = false;
    std::uint64_t probe_request_id = 4U;
    const auto close_deadline = std::chrono::steady_clock::now() + 3s;
    while (!admission_closed && std::chrono::steady_clock::now() < close_deadline)
    {
        const CommandAdmission probe_admission = ingress.submit(
            make_envelope(9U, probe_request_id, 100.0));
        if (probe_admission == CommandAdmission::closed)
        {
            admission_closed = true;
        }
        else if (probe_admission == CommandAdmission::accepted)
        {
            unexpected_probe_admission = true;
            ++probe_request_id;
        }
        else if (probe_admission == CommandAdmission::sequence_exhausted)
        {
            break;
        }
        else
        {
            std::this_thread::yield();
        }
    }

    const std::future_status shutdown_before_release = shutdown.wait_for(50ms);
    release_worker_on_exit.release();

    EXPECT_TRUE(admission_closed);
    EXPECT_FALSE(unexpected_probe_admission);
    EXPECT_EQ(shutdown_before_release, std::future_status::timeout);
    ASSERT_EQ(shutdown.wait_for(3s), std::future_status::ready);
    shutdown.get();
    worker_pool.shutdown();
    blocker_submission.future->get();
    filler_submission.future->get();

    EXPECT_EQ(blocker_executions.load(), 1U);
    EXPECT_EQ(filler_executions.load(), 1U);

    std::sort(response_ids.begin(), response_ids.end());
    EXPECT_EQ(response_ids, (std::vector<std::uint64_t>{1U, 2U}));

    const auto statistics = store.statistics_for("device-snapshot", "temperature");
    ASSERT_TRUE(statistics.has_value());
    EXPECT_EQ(statistics->sample_count, 2U);
    EXPECT_DOUBLE_EQ(statistics->minimum, 1.0);
    EXPECT_DOUBLE_EQ(statistics->maximum, 2.0);
    EXPECT_DOUBLE_EQ(statistics->mean, 1.5);
    EXPECT_DOUBLE_EQ(statistics->last_value, 2.0);
}
} // namespace
