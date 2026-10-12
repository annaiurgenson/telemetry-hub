#include <telemetryhub/network/tcp.hpp>
#include <telemetryhub/network/winsock_runtime.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <exception>
#include <future>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using telemetryhub::network::ReceiveStatus;
using telemetryhub::network::Socket;
using telemetryhub::network::WinsockRuntime;
using telemetryhub::network::accept;
using telemetryhub::network::bind_loopback;
using telemetryhub::network::connect_loopback;
using telemetryhub::network::create_ipv4_tcp_socket;
using telemetryhub::network::listen;
using telemetryhub::network::local_port;
using telemetryhub::network::receive_some;
using telemetryhub::network::send_all;

constexpr std::array ping{
    std::byte{'P'}, std::byte{'I'}, std::byte{'N'}, std::byte{'G'}};
constexpr std::array pong{
    std::byte{'P'}, std::byte{'O'}, std::byte{'N'}, std::byte{'G'}};

struct ConnectedSockets
{
    Socket client;
    Socket server;
};

[[nodiscard]] ConnectedSockets make_connected_sockets()
{
    Socket listener = create_ipv4_tcp_socket();
    bind_loopback(listener, 0U);
    listen(listener);
    const std::uint16_t port = local_port(listener);

    Socket client = create_ipv4_tcp_socket();
    connect_loopback(client, port);
    Socket server = accept(listener);
    EXPECT_FALSE(client.set_nonblocking(true));
    EXPECT_FALSE(server.set_nonblocking(true));
    return {std::move(client), std::move(server)};
}

void receive_exact_for_test(const Socket& socket, const std::span<std::byte> buffer)
{
    std::size_t total_received = 0U;
    while (total_received < buffer.size())
    {
        const auto result = receive_some(socket, buffer.subspan(total_received), std::stop_token{});
        ASSERT_EQ(result.status, ReceiveStatus::data);
        ASSERT_GT(result.bytes_transferred, 0U);
        total_received += result.bytes_transferred;
    }
}

class TcpCommunicationTest : public testing::Test
{
protected:
    WinsockRuntime runtime_;
};

TEST_F(TcpCommunicationTest, ListenerReportsAssignedEphemeralPort)
{
    Socket listener = create_ipv4_tcp_socket();
    bind_loopback(listener, 0U);

    EXPECT_NE(local_port(listener), 0U);
}

TEST_F(TcpCommunicationTest, EstablishesLoopbackClientAndServerSockets)
{
    ConnectedSockets sockets = make_connected_sockets();

    EXPECT_TRUE(sockets.client.valid());
    EXPECT_TRUE(sockets.server.valid());
}

TEST_F(TcpCommunicationTest, ExchangesRawPingAndPongBytes)
{
    ConnectedSockets sockets = make_connected_sockets();

    const auto ping_send = send_all(sockets.client, ping, std::stop_token{});
    ASSERT_FALSE(ping_send.error);
    ASSERT_EQ(ping_send.bytes_transferred, ping.size());

    std::array<std::byte, ping.size()> ping_buffer{};
    receive_exact_for_test(sockets.server, ping_buffer);
    EXPECT_EQ(ping_buffer, ping);

    const auto pong_send = send_all(sockets.server, pong, std::stop_token{});
    ASSERT_FALSE(pong_send.error);
    ASSERT_EQ(pong_send.bytes_transferred, pong.size());

    std::array<std::byte, pong.size()> pong_buffer{};
    receive_exact_for_test(sockets.client, pong_buffer);
    EXPECT_EQ(pong_buffer, pong);
}

TEST_F(TcpCommunicationTest, EmptyCompleteWriteSucceedsWithoutUsingSocket)
{
    const Socket invalid_socket;

    const auto result = send_all(invalid_socket, {}, std::stop_token{});

    EXPECT_FALSE(result.error);
    EXPECT_EQ(result.bytes_transferred, 0U);
}

TEST_F(TcpCommunicationTest, InvalidSocketSetupPreservesWinsockErrorAndContext)
{
    const Socket invalid_socket;

    try
    {
        bind_loopback(invalid_socket, 0U);
        FAIL() << "Expected std::system_error";
    }
    catch (const std::system_error& error)
    {
        EXPECT_EQ(error.code().value(), WSAENOTSOCK);
        EXPECT_STREQ(error.code().category().name(), "winsock");
        EXPECT_NE(std::string{error.what()}.find("bind"), std::string::npos);
    }
}

TEST_F(TcpCommunicationTest, BindFailureRetainsOwnershipForAutomaticCleanup)
{
    Socket first = create_ipv4_tcp_socket();
    bind_loopback(first, 0U);
    listen(first);
    const std::uint16_t occupied_port = local_port(first);

    Socket second = create_ipv4_tcp_socket();
    try
    {
        bind_loopback(second, occupied_port);
        FAIL() << "Expected std::system_error";
    }
    catch (const std::system_error& error)
    {
        EXPECT_EQ(error.code().value(), WSAEADDRINUSE);
    }

    EXPECT_TRUE(first.valid());
    EXPECT_TRUE(second.valid());
}

TEST_F(TcpCommunicationTest, InvalidSocketIoReturnsWinsockErrors)
{
    const Socket invalid_socket;
    std::array<std::byte, 1U> byte{std::byte{'X'}};

    const auto send_result = send_all(invalid_socket, byte, std::stop_token{});
    const auto receive_result = receive_some(invalid_socket, byte, std::stop_token{});

    EXPECT_EQ(send_result.bytes_transferred, 0U);
    EXPECT_EQ(send_result.error.value(), WSAENOTSOCK);
    EXPECT_EQ(receive_result.status, ReceiveStatus::error);
    EXPECT_EQ(receive_result.bytes_transferred, 0U);
    EXPECT_EQ(receive_result.error.value(), WSAENOTSOCK);
}

TEST_F(TcpCommunicationTest, EmptyReceiveBufferReportsInvalidArgument)
{
    const Socket invalid_socket;

    const auto result = receive_some(invalid_socket, {}, std::stop_token{});

    EXPECT_EQ(result.status, ReceiveStatus::error);
    EXPECT_EQ(result.bytes_transferred, 0U);
    EXPECT_EQ(result.error.value(), WSAEINVAL);
}

TEST_F(TcpCommunicationTest, BoundedReadDoesNotExceedCallerBuffer)
{
    ConnectedSockets sockets = make_connected_sockets();
    constexpr std::array payload{
        std::byte{'A'}, std::byte{'B'}, std::byte{'C'}, std::byte{'D'},
        std::byte{'E'}, std::byte{'F'}, std::byte{'G'}, std::byte{'H'}};
    ASSERT_FALSE(send_all(sockets.client, payload, std::stop_token{}).error);
    std::array<std::byte, 3U> buffer{};

    const auto result = receive_some(sockets.server, buffer, std::stop_token{});

    ASSERT_EQ(result.status, ReceiveStatus::data);
    ASSERT_GT(result.bytes_transferred, 0U);
    EXPECT_LE(result.bytes_transferred, buffer.size());
    EXPECT_TRUE(std::equal(
        buffer.begin(),
        buffer.begin() + result.bytes_transferred,
        payload.begin(),
        payload.begin() + result.bytes_transferred));
}

TEST_F(TcpCommunicationTest, PayloadCanBeAccumulatedAcrossBoundedReads)
{
    ConnectedSockets sockets = make_connected_sockets();
    constexpr std::array payload{
        std::byte{'0'}, std::byte{'1'}, std::byte{'2'}, std::byte{'3'},
        std::byte{'4'}, std::byte{'5'}, std::byte{'6'}, std::byte{'7'},
        std::byte{'8'}, std::byte{'9'}};
    ASSERT_FALSE(send_all(sockets.client, payload, std::stop_token{}).error);
    std::array<std::byte, 3U> chunk{};
    std::vector<std::byte> received;
    received.reserve(payload.size());

    while (received.size() < payload.size())
    {
        const auto result = receive_some(sockets.server, chunk, std::stop_token{});
        ASSERT_EQ(result.status, ReceiveStatus::data);
        ASSERT_GT(result.bytes_transferred, 0U);
        received.insert(
            received.end(), chunk.begin(), chunk.begin() + result.bytes_transferred);
    }

    EXPECT_EQ(received.size(), payload.size());
    EXPECT_TRUE(std::equal(received.begin(), received.end(), payload.begin(), payload.end()));
}

TEST_F(TcpCommunicationTest, GracefulPeerClosureIsDistinguishedFromError)
{
    ConnectedSockets sockets = make_connected_sockets();
    ASSERT_FALSE(sockets.client.close());
    std::array<std::byte, 1U> buffer{};

    const auto result = receive_some(sockets.server, buffer, std::stop_token{});

    EXPECT_EQ(result.status, ReceiveStatus::peer_closed);
    EXPECT_EQ(result.bytes_transferred, 0U);
    EXPECT_FALSE(result.error);
}

void saturate_send_buffer(const Socket& socket)
{
    int send_buffer_size = 1024;
    ASSERT_EQ(setsockopt(
        socket.native_handle(), SOL_SOCKET, SO_SNDBUF,
        reinterpret_cast<const char*>(&send_buffer_size), static_cast<int>(sizeof(send_buffer_size))), 0);

    std::array<char, 4096U> bytes{};
    bool reached_would_block = false;
    std::size_t total_sent = 0U;
    while (total_sent < 64U * 1024U * 1024U)
    {
        const int sent = ::send(
            socket.native_handle(), bytes.data(), static_cast<int>(bytes.size()), 0);
        if (sent == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            ASSERT_EQ(error, WSAEWOULDBLOCK);
            reached_would_block = true;
            break;
        }
        ASSERT_GT(sent, 0);
        total_sent += static_cast<std::size_t>(sent);
    }
    ASSERT_TRUE(reached_would_block) << "localhost send buffer did not reach backpressure";
}


TEST_F(TcpCommunicationTest, PollTimeoutIsAProductionRetryOutcome)
{
    using namespace std::chrono_literals;
    ConnectedSockets sockets = make_connected_sockets();
    const auto start = std::chrono::steady_clock::now();
    const auto result = telemetryhub::network::wait_for_socket(
        sockets.server, telemetryhub::network::SocketWaitInterest::read, {}, 100ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(result.status, telemetryhub::network::SocketWaitStatus::timeout);
    EXPECT_GE(elapsed, 50ms);
    EXPECT_LT(elapsed, 2s);
}

TEST_F(TcpCommunicationTest, PeerSendShutdownProducesOrderlyEof)
{
    ConnectedSockets sockets = make_connected_sockets();
    ASSERT_EQ(::shutdown(sockets.client.native_handle(), SD_SEND), 0);
    std::array<std::byte, 1U> buffer{};

    const auto result = receive_some(sockets.server, buffer, std::stop_token{});

    EXPECT_EQ(result.status, ReceiveStatus::peer_closed);
    EXPECT_FALSE(result.error);
}

TEST_F(TcpCommunicationTest, ReaderCancellationWhilePeerRemainsConnected)
{
    using namespace std::chrono_literals;
    ConnectedSockets sockets = make_connected_sockets();
    std::stop_source stop_source;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<telemetryhub::network::ReceiveResult> completion;
    auto completed = completion.get_future();
    std::jthread reader{[&] {
        try
        {
            std::array<std::byte, 1U> buffer{};
            entered.set_value();
            completion.set_value(receive_some(sockets.server, buffer, stop_source.get_token()));
        }
        catch (...)
        {
            completion.set_exception(std::current_exception());
        }
    }};

    EXPECT_EQ(entered_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(completed.wait_for(20ms), std::future_status::timeout);
    stop_source.request_stop();
    const auto status = completed.wait_for(1s);
    EXPECT_EQ(status, std::future_status::ready);
    reader.join();
    if (status == std::future_status::ready)
    {
        EXPECT_EQ(completed.get().status, ReceiveStatus::cancelled);
    }
    EXPECT_FALSE(sockets.server.close());
    EXPECT_FALSE(sockets.client.close());
}

TEST_F(TcpCommunicationTest, RepeatedReaderCancellationDoesNotLeakOrHang)
{
    using namespace std::chrono_literals;
    for (int iteration = 0; iteration < 8; ++iteration)
    {
        ConnectedSockets sockets = make_connected_sockets();
        std::stop_source stop_source;
        std::promise<telemetryhub::network::ReceiveResult> completion;
        auto completed = completion.get_future();
        std::jthread reader{[&] {
            std::array<std::byte, 1U> buffer{};
            completion.set_value(receive_some(sockets.server, buffer, stop_source.get_token()));
        }};
        stop_source.request_stop();
        ASSERT_EQ(completed.wait_for(1s), std::future_status::ready);
        reader.join();
        EXPECT_EQ(completed.get().status, ReceiveStatus::cancelled);
        EXPECT_FALSE(sockets.server.close());
        EXPECT_FALSE(sockets.client.close());
    }
}

TEST_F(TcpCommunicationTest, LargePayloadIsCompletelySentWithConstrainedSendBuffer)
{
    using namespace std::chrono_literals;
    ConnectedSockets sockets = make_connected_sockets();
    int send_buffer_size = 1024;
    ASSERT_EQ(setsockopt(
        sockets.client.native_handle(), SOL_SOCKET, SO_SNDBUF,
        reinterpret_cast<const char*>(&send_buffer_size),
        static_cast<int>(sizeof(send_buffer_size))), 0);

    std::vector<std::byte> payload(4U * 1024U * 1024U, std::byte{'Z'});
    std::stop_source transfer_stop;
    std::promise<std::vector<std::byte>> received_promise;
    auto received_future = received_promise.get_future();
    std::jthread reader{[&] {
        try
        {
            std::vector<std::byte> received;
            received.reserve(payload.size());
            std::array<std::byte, 8192U> buffer{};
            while (received.size() < payload.size())
            {
                const auto result =
                    receive_some(sockets.server, buffer, transfer_stop.get_token());
                if (result.status != ReceiveStatus::data || result.bytes_transferred == 0U)
                {
                    throw std::runtime_error("receive failed during complete-write test");
                }
                received.insert(
                    received.end(), buffer.begin(),
                    buffer.begin() + result.bytes_transferred);
            }
            received_promise.set_value(std::move(received));
        }
        catch (...)
        {
            received_promise.set_exception(std::current_exception());
        }
    }};

    const auto sent = send_all(sockets.client, payload, transfer_stop.get_token());
    EXPECT_FALSE(sent.error);
    EXPECT_FALSE(sent.cancelled);
    EXPECT_EQ(sent.bytes_transferred, payload.size());
    if (sent.error || sent.cancelled)
    {
        transfer_stop.request_stop();
    }

    auto receive_status = received_future.wait_for(5s);
    if (receive_status != std::future_status::ready)
    {
        transfer_stop.request_stop();
        receive_status = received_future.wait_for(1s);
    }
    EXPECT_EQ(receive_status, std::future_status::ready);
    reader.join();
    if (receive_status == std::future_status::ready)
    {
        try
        {
            EXPECT_EQ(received_future.get(), payload);
        }
        catch (const std::exception& error)
        {
            FAIL() << error.what();
        }
    }
}
TEST_F(TcpCommunicationTest, WriterCancellationWhenPeerDoesNotConsumeData)
{
    using namespace std::chrono_literals;
    ConnectedSockets sockets = make_connected_sockets();
    saturate_send_buffer(sockets.client);

    std::stop_source stop_source;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<telemetryhub::network::SendResult> completion;
    auto completed = completion.get_future();
    std::jthread writer{[&] {
        constexpr std::array extra{std::byte{'X'}};
        entered.set_value();
        completion.set_value(send_all(sockets.client, extra, stop_source.get_token()));
    }};

    EXPECT_EQ(entered_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(completed.wait_for(20ms), std::future_status::timeout);
    stop_source.request_stop();
    const auto status = completed.wait_for(1s);
    EXPECT_EQ(status, std::future_status::ready);
    writer.join();
    if (status == std::future_status::ready)
    {
        const auto result = completed.get();
        EXPECT_TRUE(result.cancelled);
        EXPECT_FALSE(result.error);
    }
    EXPECT_FALSE(sockets.client.close());
    EXPECT_FALSE(sockets.server.close());
}

TEST_F(TcpCommunicationTest, DynamicDeadlineInterruptsAnAlreadyBackpressuredSend)
{
    using namespace std::chrono_literals;
    ConnectedSockets sockets = make_connected_sockets();
    int receive_buffer_size = 1024;
    ASSERT_EQ(setsockopt(
        sockets.server.native_handle(), SOL_SOCKET, SO_RCVBUF,
        reinterpret_cast<const char*>(&receive_buffer_size),
        static_cast<int>(sizeof(receive_buffer_size))), 0);
    saturate_send_buffer(sockets.client);

    std::atomic<std::int64_t> deadline_ticks{0};
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<telemetryhub::network::SendResult> completion;
    auto completed = completion.get_future();
    std::jthread writer{[&] {
        entered.set_value();
        const std::vector<std::byte> extra(16U * 1024U * 1024U, std::byte{'D'});
        completion.set_value(send_all(
            sockets.client, extra, {}, std::nullopt, &deadline_ticks));
    }};

    ASSERT_EQ(entered_future.wait_for(2s), std::future_status::ready);
    const auto deadline = std::chrono::steady_clock::now() + 100ms;
    deadline_ticks.store(
        static_cast<std::int64_t>(deadline.time_since_epoch().count()), std::memory_order_release);
    ASSERT_EQ(completed.wait_for(1s), std::future_status::ready);
    const auto result = completed.get();
    writer.join();
    EXPECT_TRUE(result.timed_out);
    EXPECT_FALSE(result.error);
    EXPECT_FALSE(sockets.client.close());
    EXPECT_FALSE(sockets.server.close());
}
TEST_F(TcpCommunicationTest, ReaderAndWriterExitBeforeOwnerClosesSocket)
{
    using namespace std::chrono_literals;
    ConnectedSockets sockets = make_connected_sockets();
    saturate_send_buffer(sockets.server);
    std::stop_source stop_source;
    std::promise<telemetryhub::network::ReceiveResult> receive_completion;
    std::promise<telemetryhub::network::SendResult> send_completion;
    auto received = receive_completion.get_future();
    auto sent = send_completion.get_future();
    std::jthread reader{[&] {
        std::array<std::byte, 1U> buffer{};
        receive_completion.set_value(receive_some(sockets.server, buffer, stop_source.get_token()));
    }};
    std::jthread writer{[&] {
        constexpr std::array extra{std::byte{'X'}};
        send_completion.set_value(send_all(sockets.server, extra, stop_source.get_token()));
    }};

    stop_source.request_stop();
    ASSERT_EQ(received.wait_for(1s), std::future_status::ready);
    ASSERT_EQ(sent.wait_for(1s), std::future_status::ready);
    reader.join();
    writer.join();
    EXPECT_EQ(received.get().status, ReceiveStatus::cancelled);
    EXPECT_TRUE(sent.get().cancelled);
    EXPECT_TRUE(sockets.server.valid());
    EXPECT_FALSE(sockets.server.close());
    EXPECT_FALSE(sockets.client.close());
}
} // namespace
