#include <telemetryhub/network/tcp.hpp>
#include <telemetryhub/network/winsock_runtime.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
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
    return {std::move(client), std::move(server)};
}

void receive_exact_for_test(const Socket& socket, const std::span<std::byte> buffer)
{
    std::size_t total_received = 0U;
    while (total_received < buffer.size())
    {
        const auto result = receive_some(socket, buffer.subspan(total_received));
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

    const auto ping_send = send_all(sockets.client, ping);
    ASSERT_FALSE(ping_send.error);
    ASSERT_EQ(ping_send.bytes_transferred, ping.size());

    std::array<std::byte, ping.size()> ping_buffer{};
    receive_exact_for_test(sockets.server, ping_buffer);
    EXPECT_EQ(ping_buffer, ping);

    const auto pong_send = send_all(sockets.server, pong);
    ASSERT_FALSE(pong_send.error);
    ASSERT_EQ(pong_send.bytes_transferred, pong.size());

    std::array<std::byte, pong.size()> pong_buffer{};
    receive_exact_for_test(sockets.client, pong_buffer);
    EXPECT_EQ(pong_buffer, pong);
}

TEST_F(TcpCommunicationTest, EmptyCompleteWriteSucceedsWithoutUsingSocket)
{
    const Socket invalid_socket;

    const auto result = send_all(invalid_socket, {});

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

    const auto send_result = send_all(invalid_socket, byte);
    const auto receive_result = receive_some(invalid_socket, byte);

    EXPECT_EQ(send_result.bytes_transferred, 0U);
    EXPECT_EQ(send_result.error.value(), WSAENOTSOCK);
    EXPECT_EQ(receive_result.status, ReceiveStatus::error);
    EXPECT_EQ(receive_result.bytes_transferred, 0U);
    EXPECT_EQ(receive_result.error.value(), WSAENOTSOCK);
}

TEST_F(TcpCommunicationTest, EmptyReceiveBufferReportsInvalidArgument)
{
    const Socket invalid_socket;

    const auto result = receive_some(invalid_socket, {});

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
    ASSERT_FALSE(send_all(sockets.client, payload).error);
    std::array<std::byte, 3U> buffer{};

    const auto result = receive_some(sockets.server, buffer);

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
    ASSERT_FALSE(send_all(sockets.client, payload).error);
    std::array<std::byte, 3U> chunk{};
    std::vector<std::byte> received;
    received.reserve(payload.size());

    while (received.size() < payload.size())
    {
        const auto result = receive_some(sockets.server, chunk);
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

    const auto result = receive_some(sockets.server, buffer);

    EXPECT_EQ(result.status, ReceiveStatus::peer_closed);
    EXPECT_EQ(result.bytes_transferred, 0U);
    EXPECT_FALSE(result.error);
}

} // namespace
