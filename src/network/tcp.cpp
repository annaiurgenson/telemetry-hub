#include <telemetryhub/network/tcp.hpp>

#include <telemetryhub/network/winsock_error.hpp>

#include <winsock2.h>

#include <algorithm>
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

SocketWaitResult wait_for_socket(
    const Socket& socket,
    const SocketWaitInterest interest,
    const std::stop_token stop_token,
    const std::chrono::milliseconds timeout) noexcept
{
    if (!socket.valid())
    {
        return {SocketWaitStatus::error, make_winsock_error_code(WSAENOTSOCK)};
    }
    if (timeout.count() <= 0 ||
        timeout.count() > static_cast<std::chrono::milliseconds::rep>(
                              (std::numeric_limits<int>::max)()))
    {
        return {SocketWaitStatus::error, make_winsock_error_code(WSAEINVAL)};
    }
    if (stop_token.stop_requested())
    {
        return {SocketWaitStatus::cancelled, {}};
    }

    WSAPOLLFD descriptor{};
    descriptor.fd = socket.native_handle();
    descriptor.events = interest == SocketWaitInterest::read ? POLLRDNORM : POLLWRNORM;
    descriptor.revents = 0;
    const int result = WSAPoll(
        &descriptor, 1UL, static_cast<int>(timeout.count()));
    if (result == SOCKET_ERROR)
    {
        return {SocketWaitStatus::error, make_winsock_error_code(WSAGetLastError())};
    }
    if (stop_token.stop_requested())
    {
        return {SocketWaitStatus::cancelled, {}};
    }
    if (result == 0)
    {
        return {SocketWaitStatus::timeout, {}};
    }
    if ((descriptor.revents & POLLNVAL) != 0)
    {
        return {SocketWaitStatus::error, make_winsock_error_code(WSAENOTSOCK)};
    }
    if ((descriptor.revents & POLLERR) != 0)
    {
        return {SocketWaitStatus::error, make_winsock_error_code(WSAECONNRESET)};
    }

    const short ready_flag =
        interest == SocketWaitInterest::read ? POLLRDNORM | POLLRDBAND : POLLWRNORM;
    if ((descriptor.revents & ready_flag) != 0 ||
        (interest == SocketWaitInterest::read && (descriptor.revents & POLLHUP) != 0))
    {
        return {SocketWaitStatus::ready, {}};
    }
    return {SocketWaitStatus::timeout, {}};
}

SendResult send_all(
    const Socket& socket,
    const std::span<const std::byte> bytes,
    const std::stop_token stop_token,
    const std::optional<std::chrono::steady_clock::time_point> deadline,
    const std::atomic<std::int64_t>* dynamic_deadline_ticks) noexcept
{
    const auto current_deadline = [deadline, dynamic_deadline_ticks]() noexcept {
        auto result = deadline;
        if (dynamic_deadline_ticks != nullptr)
        {
            const std::int64_t ticks = dynamic_deadline_ticks->load(std::memory_order_acquire);
            if (ticks != 0)
            {
                using Clock = std::chrono::steady_clock;
                const auto dynamic = Clock::time_point{Clock::duration{
                    static_cast<Clock::duration::rep>(ticks)}};
                if (!result.has_value() || dynamic < *result)
                {
                    result = dynamic;
                }
            }
        }
        return result;
    };

    std::size_t total_sent = 0U;
    while (total_sent < bytes.size())
    {
        if (stop_token.stop_requested())
        {
            return {total_sent, {}, true, false};
        }
        const auto limit = current_deadline();
        if (limit.has_value() && std::chrono::steady_clock::now() >= *limit)
        {
            return {total_sent, {}, false, true};
        }

        const int chunk_size = winsock_buffer_size(bytes.size() - total_sent);
        const int sent = send(
            socket.native_handle(),
            reinterpret_cast<const char*>(bytes.data() + total_sent),
            chunk_size,
            0);
        if (sent == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK)
            {
                auto wait_timeout = default_socket_poll_interval;
                const auto refreshed_limit = current_deadline();
                if (refreshed_limit.has_value())
                {
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        *refreshed_limit - std::chrono::steady_clock::now());
                    if (remaining.count() <= 0)
                    {
                        return {total_sent, {}, false, true};
                    }
                    wait_timeout = (std::min)(wait_timeout, remaining);
                }
                const SocketWaitResult wait =
                    wait_for_socket(socket, SocketWaitInterest::write, stop_token, wait_timeout);
                if (wait.status == SocketWaitStatus::cancelled)
                {
                    return {total_sent, {}, true, false};
                }
                if (wait.status == SocketWaitStatus::error)
                {
                    return {total_sent, wait.error, false, false};
                }
                continue;
            }
            if (error == WSAEINTR)
            {
                continue;
            }
            return {total_sent, make_winsock_error_code(error), false, false};
        }
        if (sent == 0)
        {
            return {total_sent, make_winsock_error_code(WSAECONNABORTED), false, false};
        }
        total_sent += static_cast<std::size_t>(sent);
    }
    return {total_sent, {}, false, false};
}
ReceiveResult receive_some(
    const Socket& socket,
    const std::span<std::byte> buffer,
    const std::stop_token stop_token) noexcept
{
    if (buffer.empty())
    {
        return {ReceiveStatus::error, 0U, make_winsock_error_code(WSAEINVAL)};
    }

    for (;;)
    {
        if (stop_token.stop_requested())
        {
            return {ReceiveStatus::cancelled, 0U, {}};
        }

        const SocketWaitResult wait =
            wait_for_socket(socket, SocketWaitInterest::read, stop_token);
        if (wait.status == SocketWaitStatus::cancelled)
        {
            return {ReceiveStatus::cancelled, 0U, {}};
        }
        if (wait.status == SocketWaitStatus::timeout)
        {
            continue;
        }
        if (wait.status == SocketWaitStatus::error)
        {
            return {ReceiveStatus::error, 0U, wait.error};
        }

        const int received = recv(
            socket.native_handle(),
            reinterpret_cast<char*>(buffer.data()),
            winsock_buffer_size(buffer.size()),
            0);
        if (received == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINTR)
            {
                continue;
            }
            return {ReceiveStatus::error, 0U, make_winsock_error_code(error)};
        }
        if (received == 0)
        {
            return {ReceiveStatus::peer_closed, 0U, {}};
        }
        return {ReceiveStatus::data, static_cast<std::size_t>(received), {}};
    }
}

} // namespace telemetryhub::network
