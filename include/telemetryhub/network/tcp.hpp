#pragma once

#include <telemetryhub/network/socket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <system_error>

namespace telemetryhub::network {

struct SendResult
{
    std::size_t bytes_transferred;
    std::error_code error;
    bool cancelled{};
};

enum class ReceiveStatus
{
    data,
    peer_closed,
    cancelled,
    error
};

struct ReceiveResult
{
    ReceiveStatus status;
    std::size_t bytes_transferred;
    std::error_code error;
};

enum class SocketWaitInterest
{
    read,
    write
};

enum class SocketWaitStatus
{
    ready,
    timeout,
    cancelled,
    error
};

struct SocketWaitResult
{
    SocketWaitStatus status;
    std::error_code error;
};

inline constexpr std::chrono::milliseconds default_socket_poll_interval{100};

[[nodiscard]] Socket create_ipv4_tcp_socket();
void bind_loopback(const Socket& socket, std::uint16_t port);
[[nodiscard]] std::uint16_t local_port(const Socket& socket);
void listen(const Socket& socket, int backlog = 1);
[[nodiscard]] Socket accept(const Socket& listener);
void connect_loopback(const Socket& socket, std::uint16_t port);

// The socket must be nonblocking. One finite WSAPoll wait; peer-close
// readiness is returned as ready for reads so recv can distinguish EOF/reset.
[[nodiscard]] SocketWaitResult wait_for_socket(
    const Socket& socket,
    SocketWaitInterest interest,
    std::stop_token stop_token,
    std::chrono::milliseconds timeout = default_socket_poll_interval) noexcept;

// I/O helpers require a nonblocking socket and explicit stop tokens. Unstopped tokens continue polling.
// keep it alive until all borrowing I/O threads have exited and been joined.
[[nodiscard]] SendResult send_all(
    const Socket& socket,
    std::span<const std::byte> bytes,
    std::stop_token stop_token) noexcept;

// Reads at most buffer.size() bytes. A nonempty buffer is required.
[[nodiscard]] ReceiveResult receive_some(
    const Socket& socket,
    std::span<std::byte> buffer,
    std::stop_token stop_token) noexcept;

} // namespace telemetryhub::network
