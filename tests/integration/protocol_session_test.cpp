#include <telemetryhub/network/tcp.hpp>
#include <telemetryhub/network/tcp_server.hpp>
#include <telemetryhub/network/winsock_error.hpp>
#include <telemetryhub/protocol/message_codec.hpp>
#include <telemetryhub/protocol/message_framer.hpp>
#include <telemetryhub/server/command_ingress.hpp>
#include <telemetryhub/server/device_registration_registry.hpp>
#include <telemetryhub/server/protocol_session_handler.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <future>
#include <functional>
#include <latch>
#include <barrier>
#include <limits>
#include <stdexcept>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <winsock2.h>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using telemetryhub::network::ConnectionId;
using telemetryhub::domain::TelemetryProcessor;
using telemetryhub::domain::TelemetryStore;
using telemetryhub::network::ReceiveStatus;
using telemetryhub::network::Socket;
using telemetryhub::network::TcpServer;
using telemetryhub::network::TcpServerConfig;
using telemetryhub::network::connect_loopback;
using telemetryhub::network::create_ipv4_tcp_socket;
using telemetryhub::network::receive_some;
using telemetryhub::protocol::Command;
using telemetryhub::protocol::ErrorResponse;
using telemetryhub::protocol::Request;
using telemetryhub::protocol::Response;

void expect_send(Socket& socket, const std::span<const std::byte> bytes)
{
    const auto result = telemetryhub::network::send_all(socket, bytes, {});
    ASSERT_FALSE(result.error);
    ASSERT_FALSE(result.cancelled);
    ASSERT_FALSE(result.timed_out);
    ASSERT_EQ(result.bytes_transferred, bytes.size());
}

[[nodiscard]] std::vector<std::byte> make_frame(const Request& request)
{
    auto payload = telemetryhub::protocol::serialize_request_payload(request);
    if (payload.error.has_value())
    {
        throw std::runtime_error{"request payload serialization failed"};
    }
    auto frame = telemetryhub::protocol::encode_frame(payload.payload);
    if (frame.error.has_value())
    {
        throw std::runtime_error{"request framing failed"};
    }
    return std::move(frame.frame);
}

[[nodiscard]] Response read_response(Socket& socket)
{
    std::array<std::byte, telemetryhub::protocol::frame_header_size_bytes> header{};
    std::size_t received = 0U;
    while (received < header.size())
    {
        const auto result = receive_some(socket, std::span{header}.subspan(received), {});
        if (result.status != ReceiveStatus::data)
        {
            throw std::runtime_error{"connection closed before response header was complete"};
        }
        received += result.bytes_transferred;
    }

    std::size_t payload_size = 0U;
    for (const std::byte byte : header)
    {
        payload_size = (payload_size << 8U) | std::to_integer<unsigned int>(byte);
    }
    std::vector<std::byte> payload(payload_size);
    received = 0U;
    while (received < payload.size())
    {
        const auto result = receive_some(socket, std::span{payload}.subspan(received), {});
        if (result.status != ReceiveStatus::data)
        {
            throw std::runtime_error{"connection closed before response payload was complete"};
        }
        received += result.bytes_transferred;
    }

    auto parsed = telemetryhub::protocol::parse_response_payload(payload);
    if (const auto* response = std::get_if<Response>(&parsed))
    {
        return *response;
    }
    throw std::runtime_error{"server emitted a response rejected by the typed codec"};
}

bool wait_for_registration_count(
    const std::shared_ptr<telemetryhub::server::DeviceRegistrationRegistry>& registry,
    const std::size_t count)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (registry->size() == count)
        {
            return true;
        }
        std::this_thread::yield();
    }
    return registry->size() == count;
}

struct TestServer
{
    TelemetryStore store;
    TelemetryProcessor processor{store};
    telemetryhub::concurrency::WorkerPool worker_pool{
        telemetryhub::server::default_telemetry_worker_count,
        telemetryhub::server::default_worker_pool_task_queue_capacity};
    std::shared_ptr<telemetryhub::server::DeviceRegistrationRegistry> registrations =
        std::make_shared<telemetryhub::server::DeviceRegistrationRegistry>();
    std::shared_ptr<telemetryhub::server::CommandIngress> ingress;
    std::shared_ptr<telemetryhub::server::ProtocolSessionHandler> handler;
    TcpServer server;

    static TcpServerConfig make_config(
        const std::shared_ptr<telemetryhub::server::ProtocolSessionHandler>& shared_handler,
        const std::size_t outbound_capacity)
    {
        TcpServerConfig config;
        config.outbound_queue_capacity = outbound_capacity;
        config.session_callbacks_factory = [shared_handler](const ConnectionId id) {
            return shared_handler->callbacks_for(id);
        };
        return config;
    }

    explicit TestServer(
        const std::size_t incoming_capacity = telemetryhub::server::default_incoming_command_queue_capacity,
        std::function<void()> before_response = {},
        const std::size_t outbound_capacity = telemetryhub::network::default_outbound_queue_capacity,
        const std::size_t worker_count = telemetryhub::server::default_telemetry_worker_count,
        const std::size_t worker_queue_capacity = telemetryhub::server::default_worker_pool_task_queue_capacity)
        : worker_pool{worker_count, worker_queue_capacity},
          ingress{std::make_shared<telemetryhub::server::CommandIngress>(processor, worker_pool, incoming_capacity)},
          handler{std::make_shared<telemetryhub::server::ProtocolSessionHandler>(registrations, ingress)},
          server{make_config(handler, outbound_capacity)}
    {
        ingress->start([this, gate = std::move(before_response)](
                             const ConnectionId id, std::vector<std::byte> bytes) {
            if (gate)
            {
                gate();
            }
            return server.try_enqueue(id, std::move(bytes));
        });
        server.start();
    }

    ~TestServer()
    {
        ingress->shutdown();
        worker_pool.shutdown();
        server.shutdown();
    }

    [[nodiscard]] Socket client() const
    {
        Socket socket = create_ipv4_tcp_socket();
        connect_loopback(socket, server.bound_port());
        if (const auto error = socket.set_nonblocking(true))
        {
            throw std::system_error{error, "Failed to set protocol test socket nonblocking"};
        }
        return socket;
    }
};
TEST(ProtocolSession, PingSupportsFragmentationCombinedFramesAndUint64Ids)
{
    TestServer fixture;
    Socket client = fixture.client();

    const auto first = make_frame(Request{(std::numeric_limits<std::uint64_t>::max)(), Command{telemetryhub::protocol::Ping{}}});
    expect_send(client, std::span{first}.first(2U));
    expect_send(client, std::span{first}.subspan(2U));
    auto response = read_response(client);
    EXPECT_EQ(response.request_id, (std::numeric_limits<std::uint64_t>::max)());
    EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::Pong>(response.body));

    const auto second = make_frame(Request{27U, Command{telemetryhub::protocol::Ping{}}});
    const auto third = make_frame(Request{28U, Command{telemetryhub::protocol::Ping{}}});
    std::vector<std::byte> combined = second;
    combined.insert(combined.end(), third.begin(), third.end());
    expect_send(client, combined);
    EXPECT_EQ(read_response(client).request_id, 27U);
    EXPECT_EQ(read_response(client).request_id, 28U);
}

TEST(ProtocolSession, RegistrationCommandsAndProcessingUnavailableResponses)
{
    TestServer fixture;
    Socket client = fixture.client();

    expect_send(client, make_frame(Request{1U, Command{telemetryhub::protocol::RegisterDevice{"device-a"}}}));
    auto registered = read_response(client);
    ASSERT_EQ(registered.request_id, 1U);
    ASSERT_TRUE(std::holds_alternative<telemetryhub::protocol::DeviceRegistered>(registered.body));

    expect_send(client, make_frame(Request{2U, Command{telemetryhub::protocol::RegisterDevice{"device-a"}}}));
    EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::DeviceRegistered>(read_response(client).body));

    expect_send(client, make_frame(Request{3U, Command{telemetryhub::protocol::RegisterDevice{"device-b"}}}));
    auto different_device = read_response(client);
    ASSERT_TRUE(std::holds_alternative<ErrorResponse>(different_device.body));
    EXPECT_EQ(std::get<ErrorResponse>(different_device.body).code, "ALREADY_REGISTERED");

    expect_send(client, make_frame(Request{4U, Command{telemetryhub::protocol::SubmitTelemetry{"temperature", 21.5}}}));
    auto telemetry = read_response(client);
    ASSERT_EQ(telemetry.request_id, 4U);
    EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::TelemetryAccepted>(telemetry.body));

    expect_send(client, make_frame(Request{5U, Command{telemetryhub::protocol::GetStatistics{"temperature"}}}));
    auto statistics = read_response(client);
    ASSERT_EQ(statistics.request_id, 5U);
    ASSERT_TRUE(std::holds_alternative<telemetryhub::protocol::Statistics>(statistics.body));
    const auto& stats = std::get<telemetryhub::protocol::Statistics>(statistics.body);
    EXPECT_EQ(stats.metric, "temperature");
    EXPECT_EQ(stats.sample_count, 1U);
    EXPECT_DOUBLE_EQ(stats.minimum, 21.5);
    EXPECT_DOUBLE_EQ(stats.maximum, 21.5);
    EXPECT_DOUBLE_EQ(stats.mean, 21.5);
    EXPECT_DOUBLE_EQ(stats.last_value, 21.5);
    EXPECT_EQ(stats.last_received_at.size(), 24U);
}

TEST(ProtocolSession, UnknownStatisticsMetricReturnsStatisticsNotFound)
{
    TestServer fixture;
    Socket client = fixture.client();
    expect_send(client, make_frame(Request{1U, Command{telemetryhub::protocol::RegisterDevice{"missing-device"}}}));
    EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::DeviceRegistered>(read_response(client).body));
    expect_send(client, make_frame(Request{2U, Command{telemetryhub::protocol::GetStatistics{"unknown"}}}));

    const auto response = read_response(client);
    ASSERT_EQ(response.request_id, 2U);
    ASSERT_TRUE(std::holds_alternative<ErrorResponse>(response.body));
    EXPECT_EQ(std::get<ErrorResponse>(response.body).code, "STATISTICS_NOT_FOUND");
}

TEST(ProtocolSession, ConcurrentClientsKeepDeviceStatisticsIndependent)
{
    TestServer fixture;
    Socket first = fixture.client();
    Socket second = fixture.client();
    expect_send(first, make_frame(Request{1U, Command{telemetryhub::protocol::RegisterDevice{"client-a"}}}));
    expect_send(second, make_frame(Request{1U, Command{telemetryhub::protocol::RegisterDevice{"client-b"}}}));
    EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::DeviceRegistered>(read_response(first).body));
    EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::DeviceRegistered>(read_response(second).body));

    expect_send(first, make_frame(Request{2U, Command{telemetryhub::protocol::SubmitTelemetry{"m", 10.0}}}));
    expect_send(second, make_frame(Request{2U, Command{telemetryhub::protocol::SubmitTelemetry{"m", 30.0}}}));
    EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::TelemetryAccepted>(read_response(first).body));
    EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::TelemetryAccepted>(read_response(second).body));

    expect_send(first, make_frame(Request{3U, Command{telemetryhub::protocol::GetStatistics{"m"}}}));
    expect_send(second, make_frame(Request{3U, Command{telemetryhub::protocol::GetStatistics{"m"}}}));
    const auto first_stats = read_response(first);
    const auto second_stats = read_response(second);
    ASSERT_EQ(first_stats.request_id, 3U);
    ASSERT_EQ(second_stats.request_id, 3U);
    ASSERT_TRUE(std::holds_alternative<telemetryhub::protocol::Statistics>(first_stats.body));
    ASSERT_TRUE(std::holds_alternative<telemetryhub::protocol::Statistics>(second_stats.body));
    EXPECT_DOUBLE_EQ(std::get<telemetryhub::protocol::Statistics>(first_stats.body).mean, 10.0);
    EXPECT_DOUBLE_EQ(std::get<telemetryhub::protocol::Statistics>(second_stats.body).mean, 30.0);
}
TEST(ProtocolSession, UnregisteredCommandsReturnDeviceNotRegistered)
{
    TestServer fixture;
    Socket client = fixture.client();
    const auto telemetry = make_frame(Request{10U, Command{telemetryhub::protocol::SubmitTelemetry{"m", 1.0}}});
    const auto statistics = make_frame(Request{11U, Command{telemetryhub::protocol::GetStatistics{"m"}}});
    std::vector<std::byte> combined = telemetry;
    combined.insert(combined.end(), statistics.begin(), statistics.end());
    expect_send(client, combined);

    for (std::uint64_t id : {10U, 11U})
    {
        auto response = read_response(client);
        ASSERT_EQ(response.request_id, id);
        ASSERT_TRUE(std::holds_alternative<ErrorResponse>(response.body));
        EXPECT_EQ(std::get<ErrorResponse>(response.body).code, "DEVICE_NOT_REGISTERED");
    }
}

TEST(ProtocolSession, ConcurrentRegistrationHasOneOwnerAndDisconnectAllowsReconnect)
{
    TestServer fixture;
    Socket first = fixture.client();
    Socket second = fixture.client();
    const auto first_request = make_frame(Request{1U, Command{telemetryhub::protocol::RegisterDevice{"shared-device"}}});
    const auto second_request = make_frame(Request{2U, Command{telemetryhub::protocol::RegisterDevice{"shared-device"}}});

    std::barrier start_together{2};
    std::array<telemetryhub::network::SendResult, 2U> send_results{};
    std::jthread first_sender{[&] {
        start_together.arrive_and_wait();
        send_results[0] = telemetryhub::network::send_all(first, first_request, {});
    }};
    std::jthread second_sender{[&] {
        start_together.arrive_and_wait();
        send_results[1] = telemetryhub::network::send_all(second, second_request, {});
    }};
    first_sender.join();
    second_sender.join();
    ASSERT_EQ(send_results[0].bytes_transferred, first_request.size());
    ASSERT_EQ(send_results[1].bytes_transferred, second_request.size());

    auto first_response = read_response(first);
    auto second_response = read_response(second);
    const bool first_won = std::holds_alternative<telemetryhub::protocol::DeviceRegistered>(first_response.body);
    const bool second_won = std::holds_alternative<telemetryhub::protocol::DeviceRegistered>(second_response.body);
    EXPECT_NE(first_won, second_won);
    const Response& conflict = first_won ? second_response : first_response;
    ASSERT_TRUE(std::holds_alternative<ErrorResponse>(conflict.body));
    EXPECT_EQ(std::get<ErrorResponse>(conflict.body).code, "DEVICE_IN_USE");

    Socket& owner = first_won ? first : second;
    Socket& contender = first_won ? second : first;
    ASSERT_FALSE(owner.close());
    ASSERT_TRUE(wait_for_registration_count(fixture.registrations, 0U));
    const auto retry = make_frame(Request{3U, Command{telemetryhub::protocol::RegisterDevice{"shared-device"}}});
    expect_send(contender, retry);
    auto reconnected = read_response(contender);
    EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::DeviceRegistered>(reconnected.body));
}
TEST(ProtocolSession, CorrelatedValidationErrorFlushesThenClosesOnlyThatClient)
{
    TestServer fixture;
    Socket malformed = fixture.client();
    Socket healthy = fixture.client();

    const std::string invalid_request = R"({"version":1,"request_id":"99","command":"BOGUS"})";
    std::vector<std::byte> payload;
    payload.reserve(invalid_request.size());
    for (const char character : invalid_request)
    {
        payload.push_back(static_cast<std::byte>(character));
    }
    const auto frame = telemetryhub::protocol::encode_frame(payload);
    ASSERT_FALSE(frame.error.has_value());
    expect_send(malformed, frame.frame);

    auto error = read_response(malformed);
    EXPECT_EQ(error.request_id, 99U);
    ASSERT_TRUE(std::holds_alternative<ErrorResponse>(error.body));
    EXPECT_EQ(std::get<ErrorResponse>(error.body).code, "UNKNOWN_COMMAND");

    std::array<std::byte, 1U> byte{};
    const auto closed = receive_some(malformed, byte, {});
    EXPECT_EQ(closed.status, ReceiveStatus::peer_closed);

    expect_send(healthy, make_frame(Request{100U, Command{telemetryhub::protocol::Ping{}}}));
    EXPECT_EQ(read_response(healthy).request_id, 100U);
}

TEST(ProtocolSession, TerminalBatchUsesOnlyNonblockingOutboundAdmissions)
{
    auto registrations = std::make_shared<telemetryhub::server::DeviceRegistrationRegistry>();
    TelemetryStore store;
    TelemetryProcessor processor{store};
    telemetryhub::concurrency::WorkerPool worker_pool{
        telemetryhub::server::default_telemetry_worker_count,
        telemetryhub::server::default_worker_pool_task_queue_capacity};
    auto ingress = std::make_shared<telemetryhub::server::CommandIngress>(processor, worker_pool);
    telemetryhub::server::ProtocolSessionHandler handler{registrations, ingress};
    const ConnectionId connection_id{44U};
    auto callbacks = handler.callbacks_for(connection_id);

    auto combined = make_frame(Request{1U, Command{telemetryhub::protocol::Ping{}}});
    const std::string invalid_request = R"({"version":1,"request_id":"99","command":"BOGUS"})";
    std::vector<std::byte> invalid_payload;
    for (const char character : invalid_request)
    {
        invalid_payload.push_back(static_cast<std::byte>(character));
    }
    auto invalid_frame = telemetryhub::protocol::encode_frame(invalid_payload);
    ASSERT_FALSE(invalid_frame.error.has_value());
    combined.insert(combined.end(), invalid_frame.frame.begin(), invalid_frame.frame.end());

    auto output = callbacks.on_bytes(connection_id, combined);
    EXPECT_EQ(output.close_mode, telemetryhub::network::SessionCloseMode::close_after_flush);
    ASSERT_EQ(output.outbound.size(), 2U);
    for (const auto& item : output.outbound)
    {
        EXPECT_EQ(item.admission, telemetryhub::network::OutboundAdmission::try_once);
    }
}
TEST(ProtocolSession, SaturationReturnsCorrelatedServerBusyInMixedBatch)
{
    std::latch consumer_entered{1};
    std::latch release_consumer{1};
    std::atomic<bool> hold_first_response{true};
    TestServer fixture{1U, [&] {
        if (hold_first_response.exchange(false))
        {
            consumer_entered.count_down();
            release_consumer.wait();
        }
    }, 64U, 1U, 1U};
    Socket client = fixture.client();

    expect_send(client, make_frame(Request{1U, Command{telemetryhub::protocol::RegisterDevice{"queue-device"}}}));
    EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::DeviceRegistered>(read_response(client).body));
    expect_send(client, make_frame(Request{2U, Command{telemetryhub::protocol::SubmitTelemetry{"m", 1.0}}}));
    consumer_entered.wait();
    expect_send(client, make_frame(Request{3U, Command{telemetryhub::protocol::SubmitTelemetry{"m", 2.0}}}));

    std::vector<std::byte> overload_batch;
    for (std::uint64_t request_id = 5U; request_id < 33U; ++request_id)
    {
        auto frame = make_frame(Request{request_id, Command{telemetryhub::protocol::GetStatistics{"m"}}});
        overload_batch.insert(overload_batch.end(), frame.begin(), frame.end());
    }
    auto ping = make_frame(Request{33U, Command{telemetryhub::protocol::Ping{}}});
    overload_batch.insert(overload_batch.end(), ping.begin(), ping.end());
    expect_send(client, overload_batch);

    bool saw_server_busy = false;
    bool saw_pong = false;
    for (std::size_t count = 0U; count < 32U && !saw_pong; ++count)
    {
        const auto response = read_response(client);
        if (response.request_id == 33U)
        {
            saw_pong = std::holds_alternative<telemetryhub::protocol::Pong>(response.body);
        }
        else
        {
            ASSERT_TRUE(std::holds_alternative<ErrorResponse>(response.body));
            EXPECT_EQ(std::get<ErrorResponse>(response.body).code, "SERVER_BUSY");
            saw_server_busy = true;
        }
    }
    EXPECT_TRUE(saw_pong);
    EXPECT_TRUE(saw_server_busy);

    expect_send(client, make_frame(Request{34U, Command{telemetryhub::protocol::GetStatistics{"m"}}}));
    const auto busy = read_response(client);
    EXPECT_EQ(busy.request_id, 34U);
    ASSERT_TRUE(std::holds_alternative<ErrorResponse>(busy.body));
    EXPECT_EQ(std::get<ErrorResponse>(busy.body).code, "SERVER_BUSY");

    release_consumer.count_down();
    fixture.ingress->shutdown();
    fixture.worker_pool.shutdown();
    bool saw_first = false;
    bool saw_second = false;
    for (std::size_t count = 0U; count < 6U && !(saw_first && saw_second); ++count)
    {
        const auto response = read_response(client);
        if (response.request_id == 2U)
        {
            saw_first = std::holds_alternative<telemetryhub::protocol::TelemetryAccepted>(response.body);
        }
        else if (response.request_id == 3U)
        {
            saw_second = std::holds_alternative<telemetryhub::protocol::TelemetryAccepted>(response.body);
        }
        else
        {
            EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::Statistics>(response.body));
        }
    }
    EXPECT_TRUE(saw_first);
    EXPECT_TRUE(saw_second);
}
TEST(ProtocolSession, DelayedResponseForDisconnectedConnectionCannotReachReconnect)
{
    std::latch response_entered{1};
    std::latch release_response{1};
    std::atomic<bool> hold_first_response{true};
    TestServer fixture{4U, [&] {
        if (hold_first_response.exchange(false))
        {
            response_entered.count_down();
            release_response.wait();
        }
    }};

    Socket old_client = fixture.client();
    expect_send(old_client, make_frame(Request{1U, Command{telemetryhub::protocol::RegisterDevice{"reused-device"}}}));
    ASSERT_TRUE(std::holds_alternative<telemetryhub::protocol::DeviceRegistered>(read_response(old_client).body));
    const auto old_id = fixture.server.connection_ids().front();

    expect_send(old_client, make_frame(Request{2U, Command{telemetryhub::protocol::SubmitTelemetry{"m", 4.0}}}));
    response_entered.wait();
    ASSERT_FALSE(old_client.close());
    ASSERT_TRUE(wait_for_registration_count(fixture.registrations, 0U));

    Socket new_client = fixture.client();
    expect_send(new_client, make_frame(Request{3U, Command{telemetryhub::protocol::RegisterDevice{"reused-device"}}}));
    const auto registered = read_response(new_client);
    ASSERT_EQ(registered.request_id, 3U);
    ASSERT_TRUE(std::holds_alternative<telemetryhub::protocol::DeviceRegistered>(registered.body));
    const auto ids = fixture.server.connection_ids();
    ASSERT_EQ(ids.size(), 1U);
    EXPECT_NE(ids.front(), old_id);

    release_response.count_down();
    expect_send(new_client, make_frame(Request{4U, Command{telemetryhub::protocol::GetStatistics{"m"}}}));
    const auto persisted = read_response(new_client);
    ASSERT_EQ(persisted.request_id, 4U);
    ASSERT_TRUE(std::holds_alternative<telemetryhub::protocol::Statistics>(persisted.body));
    const auto& stats = std::get<telemetryhub::protocol::Statistics>(persisted.body);
    EXPECT_EQ(stats.sample_count, 1U);
    EXPECT_DOUBLE_EQ(stats.last_value, 4.0);

    expect_send(new_client, make_frame(Request{5U, Command{telemetryhub::protocol::Ping{}}}));
    const auto pong = read_response(new_client);
    EXPECT_EQ(pong.request_id, 5U);
    EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::Pong>(pong.body));
}
TEST(ProtocolSession, FatalZeroLengthFrameClosesOnlyThatConnection)
{
    TestServer fixture;
    Socket malformed = fixture.client();
    Socket healthy = fixture.client();
    const std::array<std::byte, 4U> zero_length_frame{};
    expect_send(malformed, zero_length_frame);

    std::array<std::byte, 1U> byte{};
    EXPECT_EQ(receive_some(malformed, byte, {}).status, ReceiveStatus::peer_closed);
    expect_send(healthy, make_frame(Request{102U, Command{telemetryhub::protocol::Ping{}}}));
    EXPECT_EQ(read_response(healthy).request_id, 102U);
}


TEST(ProtocolSession, IngressDrainsAcceptedTelemetryWhileServerRemainsAlive)
{
    std::latch first_response_entered{1};
    std::latch release_first_response{1};
    std::atomic<bool> hold_first{true};
    TestServer fixture{4U, [&] {
        if (hold_first.exchange(false))
        {
            first_response_entered.count_down();
            release_first_response.wait();
        }
    }};
    Socket client = fixture.client();
    expect_send(client, make_frame(Request{1U, Command{telemetryhub::protocol::RegisterDevice{"drain-device"}}}));
    EXPECT_TRUE(std::holds_alternative<telemetryhub::protocol::DeviceRegistered>(read_response(client).body));

    expect_send(client, make_frame(Request{2U, Command{telemetryhub::protocol::SubmitTelemetry{"m", 1.0}}}));
    first_response_entered.wait();
    const ConnectionId connection_id = fixture.server.connection_ids().front();
    const auto queued = fixture.ingress->submit(telemetryhub::server::CommandEnvelope{
        connection_id,
        3U,
        "drain-device",
        telemetryhub::server::ProcessingCommand{
            telemetryhub::protocol::SubmitTelemetry{"m", 3.0}},
        std::chrono::system_clock::now(),
        2U});
    ASSERT_EQ(queued, telemetryhub::server::CommandAdmission::accepted);

    std::latch shutdown_started{1};
    auto shutdown = std::async(std::launch::async, [&] {
        shutdown_started.count_down();
        fixture.ingress->shutdown();
    });
    shutdown_started.wait();
    release_first_response.count_down();
    ASSERT_EQ(shutdown.wait_for(3s), std::future_status::ready);
    shutdown.get();
    fixture.worker_pool.shutdown();

    EXPECT_TRUE(fixture.server.is_running());
    bool saw_first = false;
    bool saw_second = false;
    for (std::size_t count = 0U; count < 4U && !(saw_first && saw_second); ++count)
    {
        const auto response = read_response(client);
        if (response.request_id == 2U)
        {
            saw_first = std::holds_alternative<telemetryhub::protocol::TelemetryAccepted>(response.body);
        }
        else if (response.request_id == 3U)
        {
            saw_second = std::holds_alternative<telemetryhub::protocol::TelemetryAccepted>(response.body);
        }
    }
    EXPECT_TRUE(saw_first);
    EXPECT_TRUE(saw_second);
    const auto stats = fixture.store.statistics_for("drain-device", "m");
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->sample_count, 2U);
}
TEST(DeviceRegistrationRegistry, DeviceLookupReturnsConnectionOwnedSnapshot)
{
    telemetryhub::server::DeviceRegistrationRegistry registry;
    const ConnectionId owner{20U};
    EXPECT_EQ(registry.register_device(owner, "snapshot-device"),
              telemetryhub::server::RegistrationResult::registered);
    EXPECT_EQ(registry.device_for_connection(owner), std::optional<std::string>{"snapshot-device"});
    EXPECT_FALSE(registry.device_for_connection(ConnectionId{21U}).has_value());
}
TEST(DeviceRegistrationRegistry, StaleDisconnectCannotEraseNewOwner)
{
    telemetryhub::server::DeviceRegistrationRegistry registry;
    const ConnectionId old_connection{10U};
    const ConnectionId new_connection{11U};

    EXPECT_EQ(registry.register_device(old_connection, "reused-device"),
              telemetryhub::server::RegistrationResult::registered);
    registry.erase_connection(old_connection);
    EXPECT_EQ(registry.register_device(new_connection, "reused-device"),
              telemetryhub::server::RegistrationResult::registered);
    registry.erase_connection(old_connection);

    EXPECT_TRUE(registry.is_registered(new_connection));
    EXPECT_EQ(registry.size(), 1U);
}
TEST(ProtocolSession, MalformedJsonClosesOnlyTheAffectedConnection)
{
    TestServer fixture;
    Socket malformed = fixture.client();
    Socket healthy = fixture.client();

    const std::string invalid_json = "{bad json";
    std::vector<std::byte> payload;
    for (const char character : invalid_json)
    {
        payload.push_back(static_cast<std::byte>(character));
    }
    const auto frame = telemetryhub::protocol::encode_frame(payload);
    ASSERT_FALSE(frame.error.has_value());
    expect_send(malformed, frame.frame);

    std::array<std::byte, 1U> byte{};
    EXPECT_EQ(receive_some(malformed, byte, {}).status, ReceiveStatus::peer_closed);
    expect_send(healthy, make_frame(Request{101U, Command{telemetryhub::protocol::Ping{}}}));
    EXPECT_EQ(read_response(healthy).request_id, 101U);
}

TEST(ProtocolSession, FullOutboundQueueRejectsTerminalErrorWithoutBlocking)
{
    constexpr std::size_t large_message_size = 8U * 1024U * 1024U;
    TcpServerConfig config{0U, 1U, 1U, large_message_size};
    config.session_callbacks_factory = [large_message_size](const ConnectionId) {
        return telemetryhub::network::SessionCallbacks{
            [large_message_size](const ConnectionId, const std::span<const std::byte>) {
                telemetryhub::network::SessionOutput output;
                output.outbound.push_back(telemetryhub::network::SessionOutbound{
                    std::vector<std::byte>(large_message_size, std::byte{'x'}),
                    telemetryhub::network::OutboundAdmission::wait_for_capacity});
                output.outbound.push_back(telemetryhub::network::SessionOutbound{
                    {std::byte{'y'}}, telemetryhub::network::OutboundAdmission::wait_for_capacity});
                const auto payload = telemetryhub::protocol::serialize_response_payload(
                    Response{7U, telemetryhub::protocol::ResponseBody{
                                     ErrorResponse{"PROTOCOL_ERROR", "Bad request"}}});
                const auto frame = telemetryhub::protocol::encode_frame(payload.payload);
                output.outbound.push_back(telemetryhub::network::SessionOutbound{
                    frame.frame, telemetryhub::network::OutboundAdmission::try_once});
                output.close_mode = telemetryhub::network::SessionCloseMode::close_after_flush;
                return output;
            },
            [](const ConnectionId) {}};
    };
    TcpServer server{std::move(config)};
    server.start();
    Socket client = create_ipv4_tcp_socket();
    const int receive_buffer_size = 1024;
    ASSERT_NE(setsockopt(
                  client.native_handle(), SOL_SOCKET, SO_RCVBUF,
                  reinterpret_cast<const char*>(&receive_buffer_size),
                  static_cast<int>(sizeof(receive_buffer_size))),
              SOCKET_ERROR);
    connect_loopback(client, server.bound_port());
    ASSERT_FALSE(client.set_nonblocking(true));

    const std::array<std::byte, 1U> trigger{std::byte{'t'}};
    expect_send(client, trigger);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (server.active_connection_count() != 0U && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    EXPECT_EQ(server.active_connection_count(), 0U);
    server.shutdown();
}
TEST(ProtocolSession, FullOutboundQueueClosesOnlyTheSlowClient)
{
    constexpr std::size_t large_message_size = 8U * 1024U * 1024U;
    TcpServerConfig config{0U, 2U, 1U, large_message_size};
    config.session_callbacks_factory = [large_message_size](const ConnectionId id) {
        return telemetryhub::network::SessionCallbacks{
            [large_message_size, id](const ConnectionId, const std::span<const std::byte>) {
                telemetryhub::network::SessionOutput output;
                if (id.value() == 1U)
                {
                    output.outbound.push_back(telemetryhub::network::SessionOutbound{
                        std::vector<std::byte>(large_message_size, std::byte{'x'}),
                        telemetryhub::network::OutboundAdmission::wait_for_capacity});
                    output.outbound.push_back(telemetryhub::network::SessionOutbound{
                        {std::byte{'y'}}, telemetryhub::network::OutboundAdmission::wait_for_capacity});
                    output.outbound.push_back(telemetryhub::network::SessionOutbound{
                        {std::byte{'z'}}, telemetryhub::network::OutboundAdmission::try_once});
                    output.close_mode = telemetryhub::network::SessionCloseMode::close_after_flush;
                    return output;
                }
                output.outbound.push_back(telemetryhub::network::SessionOutbound{
                    {std::byte{'o'}, std::byte{'k'}},
                    telemetryhub::network::OutboundAdmission::wait_for_capacity});
                return output;
            },
            [](const ConnectionId) {}};
    };
    TcpServer server{std::move(config)};
    server.start();

    Socket slow_client = create_ipv4_tcp_socket();
    const int receive_buffer_size = 1024;
    ASSERT_NE(setsockopt(
                  slow_client.native_handle(), SOL_SOCKET, SO_RCVBUF,
                  reinterpret_cast<const char*>(&receive_buffer_size),
                  static_cast<int>(sizeof(receive_buffer_size))),
              SOCKET_ERROR);
    connect_loopback(slow_client, server.bound_port());
    ASSERT_FALSE(slow_client.set_nonblocking(true));
    const auto first_deadline = std::chrono::steady_clock::now() + 3s;
    while (server.active_connection_count() != 1U && std::chrono::steady_clock::now() < first_deadline)
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(server.active_connection_count(), 1U);

    Socket healthy_client = create_ipv4_tcp_socket();
    connect_loopback(healthy_client, server.bound_port());
    ASSERT_FALSE(healthy_client.set_nonblocking(true));
    const auto both_deadline = std::chrono::steady_clock::now() + 3s;
    while (server.active_connection_count() != 2U && std::chrono::steady_clock::now() < both_deadline)
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(server.active_connection_count(), 2U);

    const std::array<std::byte, 1U> trigger{std::byte{'t'}};
    expect_send(slow_client, trigger);
    const auto slow_close_deadline = std::chrono::steady_clock::now() + 5s;
    while (server.active_connection_count() != 1U && std::chrono::steady_clock::now() < slow_close_deadline)
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(server.active_connection_count(), 1U);

    expect_send(healthy_client, trigger);
    std::array<std::byte, 2U> response{};
    const auto healthy_response = receive_some(healthy_client, response, {});
    ASSERT_EQ(healthy_response.status, ReceiveStatus::data);
    ASSERT_EQ(healthy_response.bytes_transferred, response.size());
    EXPECT_EQ(response[0], std::byte{'o'});
    EXPECT_EQ(response[1], std::byte{'k'});
    server.shutdown();
}
TEST(ProtocolSession, ShutdownWaitsForActiveCallbackAndStopsBackpressuredWriter)
{
    constexpr std::size_t large_message_size = 8U * 1024U * 1024U;
    std::latch callback_entered{1};
    std::latch release_callback{1};
    std::latch shutdown_entered{1};
    std::atomic<unsigned int> disconnect_count{0U};
    TcpServerConfig config{0U, 1U, 1U, large_message_size};
    config.session_callbacks_factory = [&](const ConnectionId) {
        return telemetryhub::network::SessionCallbacks{
            [&](const ConnectionId, const std::span<const std::byte>) {
                callback_entered.count_down();
                release_callback.wait();
                telemetryhub::network::SessionOutput output;
                output.outbound.push_back(telemetryhub::network::SessionOutbound{
                    {std::byte{'r'}}, telemetryhub::network::OutboundAdmission::wait_for_capacity});
                return output;
            },
            [&](const ConnectionId) { ++disconnect_count; }};
    };
    TcpServer server{std::move(config)};
    server.start();
    Socket client = create_ipv4_tcp_socket();
    const int receive_buffer_size = 1024;
    ASSERT_NE(setsockopt(
                  client.native_handle(), SOL_SOCKET, SO_RCVBUF,
                  reinterpret_cast<const char*>(&receive_buffer_size),
                  static_cast<int>(sizeof(receive_buffer_size))),
              SOCKET_ERROR);
    connect_loopback(client, server.bound_port());
    ASSERT_FALSE(client.set_nonblocking(true));
    const auto connection_deadline = std::chrono::steady_clock::now() + 3s;
    while (server.connection_ids().empty() && std::chrono::steady_clock::now() < connection_deadline)
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(server.connection_ids().size(), 1U);
    const auto connection_id = server.connection_ids().front();
    ASSERT_EQ(server.enqueue(connection_id, std::vector<std::byte>(large_message_size, std::byte{'w'})),
              telemetryhub::network::EnqueueStatus::accepted);

    const std::array<std::byte, 1U> trigger{std::byte{'t'}};
    expect_send(client, trigger);
    callback_entered.wait();
    auto shutdown = std::async(std::launch::async, [&] {
        shutdown_entered.count_down();
        server.shutdown();
    });
    shutdown_entered.wait();
    release_callback.count_down();
    ASSERT_EQ(shutdown.wait_for(5s), std::future_status::ready);
    shutdown.get();
    EXPECT_EQ(disconnect_count.load(), 1U);
    EXPECT_FALSE(server.is_running());
}
TEST(ProtocolSession, ServerShutdownWhileSessionsAreActiveCompletes)
{
    TestServer fixture;
    Socket client = fixture.client();
    const auto response = make_frame(Request{1U, Command{telemetryhub::protocol::Ping{}}});
    expect_send(client, response);
    EXPECT_EQ(read_response(client).request_id, 1U);

    fixture.server.shutdown();
    EXPECT_EQ(fixture.registrations->size(), 0U);
    EXPECT_FALSE(fixture.server.is_running());
}

} // namespace
