#include <telemetryhub/network/tcp_server.hpp>
#include <telemetryhub/network/tcp.hpp>
#include <telemetryhub/network/winsock_error.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <thread>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using telemetryhub::network::ConnectionId;
using telemetryhub::network::EnqueueStatus;
using telemetryhub::network::Socket;
using telemetryhub::network::TcpServer;
using telemetryhub::network::TcpServerConfig;
using telemetryhub::network::accept;
using telemetryhub::network::bind_loopback;
using telemetryhub::network::connect_loopback;
using telemetryhub::network::create_ipv4_tcp_socket;
using telemetryhub::network::listen;
using telemetryhub::network::local_port;
using telemetryhub::network::make_winsock_error_code;
using telemetryhub::network::receive_some;

bool wait_for_count(const TcpServer& server, const std::size_t count)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (server.active_connection_count() == count)
        {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return server.active_connection_count() == count;
}

void set_receive_buffer(Socket& socket, const int size)
{
    if (setsockopt(
            socket.native_handle(),
            SOL_SOCKET,
            SO_RCVBUF,
            reinterpret_cast<const char*>(&size),
            static_cast<int>(sizeof(size))) == SOCKET_ERROR)
    {
        throw std::system_error{
            make_winsock_error_code(WSAGetLastError()),
            "Failed to constrain test client receive buffer"};
    }
}

bool wait_for_stopped(const TcpServer& server)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!server.is_running())
        {
            return true;
        }
        std::this_thread::yield();
    }
    return !server.is_running();
}

Socket connect_client(const TcpServer& server)
{
    Socket client = create_ipv4_tcp_socket();
    connect_loopback(client, server.bound_port());
    if (const auto error = client.set_nonblocking(true))
    {
        throw std::system_error{error, "Failed to set test client nonblocking"};
    }
    return client;
}

TEST(TcpServer, StartsOnEphemeralPortAndStopsIdempotently)
{
    TcpServer server;
    server.start();

    EXPECT_TRUE(server.is_running());
    EXPECT_NE(server.bound_port(), 0U);

    server.shutdown();
    server.shutdown();
    EXPECT_FALSE(server.is_running());
}

TEST(TcpServer, AcceptsSeveralClientsWithUniqueServerGeneratedIds)
{
    TcpServer server;
    server.start();
    std::vector<Socket> clients;
    for (int index = 0; index < 4; ++index)
    {
        clients.push_back(connect_client(server));
    }

    ASSERT_TRUE(wait_for_count(server, clients.size()));
    const auto ids = server.connection_ids();
    ASSERT_EQ(ids.size(), clients.size());
    for (std::size_t first = 0; first < ids.size(); ++first)
    {
        for (std::size_t second = first + 1U; second < ids.size(); ++second)
        {
            EXPECT_NE(ids[first], ids[second]);
        }
        EXPECT_GT(ids[first].value(), 0U);
    }
}

TEST(TcpServer, RejectsConnectionsBeyondConfiguredLimit)
{
    TcpServer server{TcpServerConfig{0U, 1U, 2U}};
    server.start();
    Socket first = connect_client(server);
    ASSERT_TRUE(wait_for_count(server, 1U));

    Socket excess = connect_client(server);
    std::array<std::byte, 1U> rejected_bytes{};
    const auto rejected = receive_some(excess, rejected_bytes, {});
    EXPECT_EQ(rejected.status, telemetryhub::network::ReceiveStatus::peer_closed);
    EXPECT_EQ(server.active_connection_count(), 1U);

    first = Socket{};
    ASSERT_TRUE(wait_for_count(server, 0U));
    Socket replacement = connect_client(server);
    EXPECT_TRUE(wait_for_count(server, 1U));
}

TEST(TcpServer, ReapsPeerClosureAndDoesNotReturnStaleConnection)
{
    TcpServer server;
    server.start();
    Socket client = connect_client(server);
    ASSERT_TRUE(wait_for_count(server, 1U));
    const ConnectionId old_id = server.connection_ids().front();

    EXPECT_FALSE(client.close());
    ASSERT_TRUE(wait_for_count(server, 0U));
    EXPECT_EQ(server.enqueue(old_id, {}), EnqueueStatus::unknown_connection);
}

TEST(TcpServer, RejectsOutboundBuffersAboveConfiguredByteLimit)
{
    TcpServer server{TcpServerConfig{0U, 1U, 2U, 4U}};
    server.start();
    Socket client = connect_client(server);
    ASSERT_TRUE(wait_for_count(server, 1U));

    const ConnectionId id = server.connection_ids().front();
    EXPECT_EQ(
        server.enqueue(id, std::vector<std::byte>(5U, std::byte{'x'})),
        EnqueueStatus::too_large);
}
TEST(TcpServer, WriterDeliversOwnedBytesEnqueuedByConnectionId)
{
    TcpServer server;
    server.start();
    Socket client = connect_client(server);
    ASSERT_TRUE(wait_for_count(server, 1U));
    const ConnectionId id = server.connection_ids().front();
    const std::vector<std::byte> payload{std::byte{'o'}, std::byte{'k'}};

    EXPECT_EQ(server.enqueue(id, payload), EnqueueStatus::accepted);
    std::array<std::byte, 8U> received{};
    const auto result = receive_some(client, received, {});
    ASSERT_EQ(result.status, telemetryhub::network::ReceiveStatus::data);
    ASSERT_EQ(result.bytes_transferred, payload.size());
    EXPECT_EQ(received[0], payload[0]);
    EXPECT_EQ(received[1], payload[1]);
}

TEST(TcpServer, ShutdownCancelsIdleReaderAndWriterWaitingForWork)
{
    TcpServer server;
    server.start();
    Socket client = connect_client(server);
    ASSERT_TRUE(wait_for_count(server, 1U));

    auto completion = std::async(std::launch::async, [&server] { server.shutdown(); });
    EXPECT_EQ(completion.wait_for(2s), std::future_status::ready);
    completion.get();
    EXPECT_FALSE(server.is_running());
}

TEST(TcpServer, ShutdownCompletesWithActiveClientsAndBackpressuredWriter)
{
    TcpServer server{TcpServerConfig{0U, 1U, 2U, 8U * 1024U * 1024U}};
    server.start();
    Socket client = connect_client(server);
    ASSERT_TRUE(wait_for_count(server, 1U));
    const ConnectionId id = server.connection_ids().front();
    std::vector<std::byte> payload(8U * 1024U * 1024U, std::byte{'x'});
    EXPECT_EQ(server.enqueue(id, std::move(payload)), EnqueueStatus::accepted);

    server.shutdown();
    EXPECT_EQ(server.active_connection_count(), 0U);
    EXPECT_FALSE(server.is_running());
}

TEST(TcpServer, EnqueueAfterShutdownAdmissionClosesReturnsClosed)
{
    TcpServer server;
    server.start();
    Socket client = connect_client(server);
    ASSERT_TRUE(wait_for_count(server, 1U));
    const ConnectionId id = server.connection_ids().front();

    std::latch shutdown_entered{1};
    auto shutdown = std::async(std::launch::async, [&server, &shutdown_entered] {
        shutdown_entered.count_down();
        server.shutdown();
    });
    shutdown_entered.wait();
    ASSERT_TRUE(wait_for_stopped(server));

    EXPECT_EQ(server.enqueue(id, {std::byte{'x'}}), EnqueueStatus::closed);
    ASSERT_EQ(shutdown.wait_for(3s), std::future_status::ready);
    shutdown.get();
    EXPECT_EQ(server.shutdown_error(), std::error_code{});
}

TEST(TcpServer, ShutdownReleasesEnqueueWaitingBehindFullOutboundQueue)
{
    constexpr std::size_t payload_size = 16U * 1024U * 1024U;
    TcpServer server{TcpServerConfig{0U, 1U, 1U, payload_size}};
    server.start();
    Socket client = connect_client(server);
    set_receive_buffer(client, 1024);
    ASSERT_TRUE(wait_for_count(server, 1U));
    const ConnectionId id = server.connection_ids().front();

    const std::vector<std::byte> payload(payload_size, std::byte{'x'});
    ASSERT_EQ(server.enqueue(id, payload), EnqueueStatus::accepted);
    ASSERT_EQ(server.enqueue(id, payload), EnqueueStatus::accepted);

    std::latch enqueue_started{1};
    auto blocked_enqueue = std::async(std::launch::async, [&server, id, &payload, &enqueue_started] {
        enqueue_started.count_down();
        return server.enqueue(id, payload);
    });
    enqueue_started.wait();

    auto shutdown = std::async(std::launch::async, [&server] { server.shutdown(); });
    ASSERT_EQ(blocked_enqueue.wait_for(5s), std::future_status::ready);
    const auto enqueue_result = blocked_enqueue.get();
    EXPECT_TRUE(enqueue_result == EnqueueStatus::closed ||
                enqueue_result == EnqueueStatus::accepted);
    ASSERT_EQ(shutdown.wait_for(5s), std::future_status::ready);
    shutdown.get();
    EXPECT_EQ(server.active_connection_count(), 0U);
}

TEST(TcpServer, ShutdownWaitsForAdmittedEnqueuesBeforeReleasingSessionOwnership)
{
    constexpr std::size_t payload_size = 16U * 1024U * 1024U;
    TcpServer server{TcpServerConfig{0U, 1U, 1U, payload_size}};
    server.start();
    Socket client = connect_client(server);
    set_receive_buffer(client, 1024);
    ASSERT_TRUE(wait_for_count(server, 1U));
    const ConnectionId id = server.connection_ids().front();

    const std::vector<std::byte> payload(payload_size, std::byte{'y'});
    ASSERT_EQ(server.enqueue(id, payload), EnqueueStatus::accepted);
    ASSERT_EQ(server.enqueue(id, payload), EnqueueStatus::accepted);

    std::latch enqueue_started{1};
    auto enqueue = std::async(std::launch::async, [&server, id, &payload, &enqueue_started] {
        enqueue_started.count_down();
        return server.enqueue(id, payload);
    });
    enqueue_started.wait();

    server.shutdown();
    ASSERT_EQ(enqueue.wait_for(3s), std::future_status::ready);
    static_cast<void>(enqueue.get());
    EXPECT_EQ(server.enqueue(id, payload), EnqueueStatus::closed);
    EXPECT_EQ(server.shutdown_error(), std::error_code{});
}

TEST(TcpServer, EnqueueDisconnectReapingAndShutdownCanRace)
{
    TcpServer server{TcpServerConfig{0U, 1U, 2U, 1024U}};
    server.start();
    Socket client = connect_client(server);
    ASSERT_TRUE(wait_for_count(server, 1U));
    const ConnectionId id = server.connection_ids().front();

    constexpr std::size_t producer_count = 4U;
    std::latch producers_ready{static_cast<std::ptrdiff_t>(producer_count)};
    std::vector<std::jthread> producers;
    producers.reserve(producer_count);
    for (std::size_t index = 0U; index < producer_count; ++index)
    {
        producers.emplace_back([&server, id, &producers_ready](const std::stop_token token) {
            producers_ready.count_down();
            while (!token.stop_requested())
            {
                const auto status = server.enqueue(id, {std::byte{'z'}}, token);
                if (status != EnqueueStatus::accepted)
                {
                    break;
                }
            }
        });
    }
    producers_ready.wait();

    ASSERT_FALSE(client.close());
    auto shutdown = std::async(std::launch::async, [&server] { server.shutdown(); });
    ASSERT_EQ(shutdown.wait_for(5s), std::future_status::ready);
    shutdown.get();
    for (auto& producer : producers)
    {
        producer.request_stop();
        producer.join();
    }
    EXPECT_EQ(server.active_connection_count(), 0U);
    EXPECT_EQ(server.shutdown_error(), std::error_code{});
}

TEST(TcpServer, PeerClosureBeforeServerShutdownIsAnExpectedSocketError)
{
    TcpServer server;
    server.start();
    Socket client = connect_client(server);
    ASSERT_TRUE(wait_for_count(server, 1U));

    ASSERT_FALSE(client.close());
    ASSERT_TRUE(wait_for_count(server, 0U));
    server.shutdown();

    EXPECT_EQ(server.shutdown_error(), std::error_code{});
}

TEST(TcpServer, SupportsRepeatedStartStopCyclesWithFreshServers)
{
    for (int iteration = 0; iteration < 5; ++iteration)
    {
        TcpServer server;
        server.start();
        EXPECT_NE(server.bound_port(), 0U);
        server.shutdown();
    }
}

TEST(TcpServer, ConcurrentShutdownRequestsAreSafe)
{
    TcpServer server;
    server.start();
    Socket client = connect_client(server);
    ASSERT_TRUE(wait_for_count(server, 1U));

    std::array<std::future<void>, 4U> shutdowns;
    for (auto& shutdown : shutdowns)
    {
        shutdown = std::async(std::launch::async, [&server] { server.shutdown(); });
    }
    for (auto& shutdown : shutdowns)
    {
        EXPECT_EQ(shutdown.wait_for(2s), std::future_status::ready);
        shutdown.get();
    }
    EXPECT_EQ(server.active_connection_count(), 0U);
}

TEST(TcpServer, CanRestartAfterShutdownWithoutReusingConnectionIds)
{
    TcpServer server;
    server.start();
    Socket first = connect_client(server);
    ASSERT_TRUE(wait_for_count(server, 1U));
    const auto first_id = server.connection_ids().front();
    server.shutdown();

    server.start();
    Socket second = connect_client(server);
    ASSERT_TRUE(wait_for_count(server, 1U));
    const auto second_id = server.connection_ids().front();

    EXPECT_GT(second_id.value(), first_id.value());
}

} // namespace