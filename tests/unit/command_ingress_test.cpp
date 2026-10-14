#include <telemetryhub/server/command_ingress.hpp>

#include <telemetryhub/protocol/message_codec.hpp>
#include <telemetryhub/protocol/message_framer.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <future>
#include <latch>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using telemetryhub::network::ConnectionId;
using telemetryhub::protocol::ErrorResponse;
using telemetryhub::protocol::Response;
using telemetryhub::server::CommandAdmission;
using telemetryhub::server::CommandEnvelope;
using telemetryhub::server::CommandIngress;

[[nodiscard]] CommandEnvelope make_envelope(
    const std::uint64_t connection_id,
    const std::uint64_t request_id)
{
    return CommandEnvelope{
        ConnectionId{connection_id}, request_id, "device-snapshot",
        telemetryhub::server::ProcessingCommand{
            telemetryhub::protocol::SubmitTelemetry{"temperature", 21.5}}};
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

TEST(CommandIngress, AcceptsOwnedEnvelopeAndReturnsCorrelatedUnavailableResponse)
{
    CommandIngress ingress{2U};
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
    ASSERT_TRUE(std::holds_alternative<ErrorResponse>(response.body));
    EXPECT_EQ(std::get<ErrorResponse>(response.body).code, "PROCESSING_UNAVAILABLE");
    ingress.shutdown();
}

TEST(CommandIngress, BoundedQueueReportsFullAndShutdownDrainsAcceptedCommands)
{
    CommandIngress ingress{1U};
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

    EXPECT_EQ(ingress.submit(make_envelope(1U, 1U)), CommandAdmission::accepted);
    first_response_entered.wait();
    EXPECT_EQ(ingress.submit(make_envelope(1U, 2U)), CommandAdmission::accepted);
    EXPECT_EQ(ingress.submit(make_envelope(1U, 3U)), CommandAdmission::full);

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
}

TEST(CommandIngress, ConcurrentShutdownCallsAreIdempotentAndDrainOnce)
{
    CommandIngress ingress{2U};
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
}
TEST(CommandIngress, ConcurrentProducerStopsAtClosedAdmission)
{
    CommandIngress ingress{8U};
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
    std::jthread producer{[&] {
        std::uint64_t request_id = 1U;
        while (request_id <= 10'000U)
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
            else
            {
                if (!reported_full.exchange(true))
                {
                    producer_saw_full.count_down();
                }
                std::this_thread::yield();
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
    producer.join();

    EXPECT_GT(accepted.load(), 1U);
    EXPECT_EQ(consumed.load(), accepted.load());
    EXPECT_EQ(ingress.submit(make_envelope(2U, 10'001U)), CommandAdmission::closed);
}
} // namespace
