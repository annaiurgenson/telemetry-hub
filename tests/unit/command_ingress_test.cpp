#include <telemetryhub/domain/telemetry_processor.hpp>
#include <telemetryhub/domain/telemetry_store.hpp>
#include <telemetryhub/protocol/message_codec.hpp>
#include <telemetryhub/protocol/message_framer.hpp>
#include <telemetryhub/server/command_ingress.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
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
    CommandIngress ingress{processor, 2U};
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
}

TEST(CommandIngress, BoundedQueueReportsFullAndShutdownDrainsAcceptedCommands)
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    CommandIngress ingress{processor, 1U};
    std::latch first_response_entered{1};
    std::latch release_first_response{1};
    std::latch second_response_completed{1};
    std::atomic<unsigned int> response_count{0U};
    std::atomic<bool> hold_first{true};
    ingress.start([&](const ConnectionId, std::vector<std::byte>) {
        if (hold_first.exchange(false))
        {
            first_response_entered.count_down();
            release_first_response.wait();
        }
        if (response_count.fetch_add(1U) == 1U)
        {
            second_response_completed.count_down();
        }
        return telemetryhub::network::EnqueueStatus::accepted;
    });

    EXPECT_EQ(ingress.submit(make_envelope(1U, 1U, 1.0)), CommandAdmission::accepted);
    first_response_entered.wait();
    EXPECT_EQ(ingress.submit(make_envelope(1U, 2U, 2.0)), CommandAdmission::accepted);
    EXPECT_EQ(ingress.submit(make_envelope(1U, 3U, 300.0)), CommandAdmission::full);

    std::latch shutdown_started{1};
    auto shutdown = std::async(std::launch::async, [&] {
        shutdown_started.count_down();
        ingress.shutdown();
    });
    shutdown_started.wait();
    release_first_response.count_down();
    ASSERT_EQ(shutdown.wait_for(3s), std::future_status::ready);
    shutdown.get();
    EXPECT_EQ(response_count.load(), 2U);
    EXPECT_EQ(ingress.submit(make_envelope(1U, 4U)), CommandAdmission::closed);
    EXPECT_EQ(second_response_completed.try_wait(), true);
    const auto statistics = store.statistics_for("device-snapshot", "temperature");
    ASSERT_TRUE(statistics.has_value());
    EXPECT_EQ(statistics->sample_count, 2U);
    EXPECT_DOUBLE_EQ(statistics->last_value, 2.0);
}

TEST(CommandIngress, ConcurrentShutdownCallsAreIdempotentAndDrainOnce)
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    CommandIngress ingress{processor, 2U};
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

    EXPECT_EQ(response_count.load(), 1U);
    EXPECT_EQ(ingress.submit(make_envelope(3U, 2U)), CommandAdmission::closed);
    EXPECT_EQ(store.statistics_for("device-snapshot", "temperature")->sample_count, 1U);
}

TEST(CommandIngress, ConcurrentProducerStopsAtClosedAdmission)
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    CommandIngress ingress{processor, 8U};
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
    EXPECT_EQ(consumed.load(), accepted.load());
    EXPECT_EQ(store.statistics_for("device-snapshot", "temperature")->sample_count, accepted.load());
}

TEST(CommandIngress, SinkExceptionIsNotRetriedAndLaterCommandsStillProcess)
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    CommandIngress ingress{processor, 4U};
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
    CommandIngress ingress{processor, producer_count * submissions_per_producer};
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
                const auto request_id = static_cast<std::uint64_t>(
                    producer * submissions_per_producer + index + 1U);
                const auto status = ingress.submit(make_envelope(
                    static_cast<std::uint64_t>(producer + 1U), request_id,
                    static_cast<double>(producer)));
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

    EXPECT_EQ(rejected.load(), 0U);
    EXPECT_EQ(responses.load(), producer_count * submissions_per_producer);
    const auto stats = store.statistics_for("device-snapshot", "temperature");
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->sample_count, producer_count * submissions_per_producer);
    EXPECT_DOUBLE_EQ(stats->minimum, 0.0);
    EXPECT_DOUBLE_EQ(stats->maximum, static_cast<double>(producer_count - 1U));
}

TEST(CommandIngress, RejectedFullQueueSubmissionDoesNotAlterLaterStatistics)
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    CommandIngress ingress{processor, 1U};
    std::latch first_response_entered{1};
    std::latch release_first_response{1};
    std::latch second_response_completed{1};
    std::latch third_response_completed{1};
    std::atomic<unsigned int> response_count{0U};
    ingress.start([&](const ConnectionId, std::vector<std::byte>) {
        const unsigned int current = response_count.fetch_add(1U);
        if (current == 0U)
        {
            first_response_entered.count_down();
            release_first_response.wait();
        }
        if (current == 1U)
        {
            second_response_completed.count_down();
        }
        if (current == 2U)
        {
            third_response_completed.count_down();
        }
        return telemetryhub::network::EnqueueStatus::accepted;
    });

    EXPECT_EQ(ingress.submit(make_envelope(1U, 1U, 1.0)), CommandAdmission::accepted);
    first_response_entered.wait();
    EXPECT_EQ(ingress.submit(make_envelope(1U, 2U, 2.0)), CommandAdmission::accepted);
    EXPECT_EQ(ingress.submit(make_envelope(1U, 3U, 300.0)), CommandAdmission::full);
    release_first_response.count_down();
    second_response_completed.wait();
    EXPECT_EQ(ingress.submit(make_envelope(1U, 4U, 4.0)), CommandAdmission::accepted);
    third_response_completed.wait();
    ingress.shutdown();

    const auto statistics = store.statistics_for("device-snapshot", "temperature");
    ASSERT_TRUE(statistics.has_value());
    EXPECT_EQ(statistics->sample_count, 3U);
    EXPECT_DOUBLE_EQ(statistics->last_value, 4.0);
}
} // namespace
