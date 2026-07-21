#include <telemetryhub/network/tcp.hpp>

#include <telemetryhub/network/winsock_error.hpp>

#include <winsock2.h>

#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace telemetryhub::network {
namespace {

[[noreturn]] void throw_last_winsock_error(const std::string_view context)
{
    const int error = WSAGetLastError();
    throw std::system_error{make_winsock_error_code(error), std::string{context}};
}

[[nodiscard]] int winsock_buffer_size(const std::size_t size) noexcept
{
    constexpr auto max_winsock_size =
        static_cast<std::size_t>((std::numeric_limits<int>::max)());
    return static_cast<int>(size < max_winsock_size ? size : max_winsock_size);
}

[[nodiscard]] sockaddr_in loopback_address(const std::uint16_t port) noexcept
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

} // namespace

Socket create_ipv4_tcp_socket()
{
    const SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET)
    {
        throw_last_winsock_error("Failed to create IPv4 TCP socket");
    }
    return Socket{handle};
}

void bind_loopback(const Socket& socket, const std::uint16_t port)
{
    const sockaddr_in address = loopback_address(port);
    if (bind(
            socket.native_handle(),
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address))) == SOCKET_ERROR)
    {
        throw_last_winsock_error("Failed to bind IPv4 loopback socket");
    }
}

std::uint16_t local_port(const Socket& socket)
{
    sockaddr_in address{};
    int address_size = static_cast<int>(sizeof(address));
    if (getsockname(
            socket.native_handle(),
            reinterpret_cast<sockaddr*>(&address),
            &address_size) == SOCKET_ERROR)
    {
        throw_last_winsock_error("Failed to retrieve local socket port");
    }
    return ntohs(address.sin_port);
}

void listen(const Socket& socket, const int backlog)
{
    if (::listen(socket.native_handle(), backlog) == SOCKET_ERROR)
    {
        throw_last_winsock_error("Failed to listen on socket");
    }
}

Socket accept(const Socket& listener)
{
    const SOCKET handle = ::accept(listener.native_handle(), nullptr, nullptr);
    if (handle == INVALID_SOCKET)
    {
        throw_last_winsock_error("Failed to accept TCP connection");
    }
    return Socket{handle};
}

void connect_loopback(const Socket& socket, const std::uint16_t port)
{
    const sockaddr_in address = loopback_address(port);
    if (connect(
            socket.native_handle(),
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address))) == SOCKET_ERROR)
    {
        throw_last_winsock_error("Failed to connect to IPv4 loopback listener");
    }
}

SendResult send_all(const Socket& socket, const std::span<const std::byte> bytes) noexcept
{
    std::size_t total_sent = 0U;
    while (total_sent < bytes.size())
    {
        const int chunk_size = winsock_buffer_size(bytes.size() - total_sent);
        const int sent = send(
            socket.native_handle(),
            reinterpret_cast<const char*>(bytes.data() + total_sent),
            chunk_size,
            0);
        if (sent == SOCKET_ERROR)
        {
            return {total_sent, make_winsock_error_code(WSAGetLastError())};
        }
        if (sent == 0)
        {
            return {total_sent, make_winsock_error_code(WSAECONNABORTED)};
        }
        total_sent += static_cast<std::size_t>(sent);
    }
    return {total_sent, {}};
}

ReceiveResult receive_some(const Socket& socket, const std::span<std::byte> buffer) noexcept
{
    if (buffer.empty())
    {
        return {ReceiveStatus::error, 0U, make_winsock_error_code(WSAEINVAL)};
    }

    const int received = recv(
        socket.native_handle(),
        reinterpret_cast<char*>(buffer.data()),
        winsock_buffer_size(buffer.size()),
        0);
    if (received == SOCKET_ERROR)
    {
        return {ReceiveStatus::error, 0U, make_winsock_error_code(WSAGetLastError())};
    }
    if (received == 0)
    {
        return {ReceiveStatus::peer_closed, 0U, {}};
    }
    return {ReceiveStatus::data, static_cast<std::size_t>(received), {}};
}

} // namespace telemetryhub::network
