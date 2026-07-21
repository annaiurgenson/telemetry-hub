#pragma once

#include <telemetryhub/network/socket.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>

namespace telemetryhub::network {

struct SendResult
{
    std::size_t bytes_transferred;
    std::error_code error;
};

enum class ReceiveStatus
{
    data,
    peer_closed,
    error
};

struct ReceiveResult
{
    ReceiveStatus status;
    std::size_t bytes_transferred;
    std::error_code error;
};

[[nodiscard]] Socket create_ipv4_tcp_socket();
void bind_loopback(const Socket& socket, std::uint16_t port);
[[nodiscard]] std::uint16_t local_port(const Socket& socket);
void listen(const Socket& socket, int backlog = 1);
[[nodiscard]] Socket accept(const Socket& listener);
void connect_loopback(const Socket& socket, std::uint16_t port);

// Returns success only after every supplied byte has been sent. On failure,
// bytes_transferred reports the successfully sent prefix.
[[nodiscard]] SendResult send_all(
    const Socket& socket, std::span<const std::byte> bytes) noexcept;

// Reads at most buffer.size() bytes. A nonempty buffer is required so a
// zero-byte recv result can unambiguously represent graceful peer closure.
[[nodiscard]] ReceiveResult receive_some(
    const Socket& socket, std::span<std::byte> buffer) noexcept;

} // namespace telemetryhub::network
