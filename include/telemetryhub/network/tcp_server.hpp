#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <system_error>
#include <vector>

namespace telemetryhub::network {

class ConnectionId
{
public:
    explicit constexpr ConnectionId(std::uint64_t value) noexcept : value_{value} {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    friend constexpr bool operator==(ConnectionId, ConnectionId) noexcept = default;

private:
    std::uint64_t value_;
};

inline constexpr std::size_t default_max_active_connections = 64U;
inline constexpr std::size_t default_outbound_queue_capacity = 16U;
inline constexpr std::size_t default_max_outbound_buffer_size_bytes = 65'540U;
inline constexpr std::chrono::milliseconds default_terminal_flush_timeout{1'000};

enum class EnqueueStatus
{
    accepted,
    unknown_connection,
    too_large,
    full,
    closed,
    cancelled
};

enum class SessionCloseMode
{
    keep_open,
    close_now,
    close_after_flush
};

enum class OutboundAdmission
{
    wait_for_capacity,
    try_once
};

struct SessionOutbound
{
    std::vector<std::byte> bytes;
    OutboundAdmission admission{OutboundAdmission::wait_for_capacity};
};

struct SessionOutput
{
    std::vector<SessionOutbound> outbound;
    SessionCloseMode close_mode{SessionCloseMode::keep_open};
};

// The input span is borrowed for the duration of on_bytes. Callbacks run
// synchronously on the reader or final I/O thread and must not block on server
// lifecycle operations or retain session/socket references.
struct SessionCallbacks
{
    std::function<SessionOutput(ConnectionId, std::span<const std::byte>)> on_bytes;
    std::function<void(ConnectionId)> on_disconnected;
};

using SessionCallbacksFactory = std::function<SessionCallbacks(ConnectionId)>;

struct TcpServerConfig
{
    std::uint16_t port{0U};
    std::size_t max_connections{default_max_active_connections};
    std::size_t outbound_queue_capacity{default_outbound_queue_capacity};
    std::size_t max_outbound_buffer_size_bytes{default_max_outbound_buffer_size_bytes};
    std::chrono::milliseconds terminal_flush_timeout{default_terminal_flush_timeout};
    SessionCallbacksFactory session_callbacks_factory;
};

class TcpServer
{
public:
    explicit TcpServer(TcpServerConfig config = {});
    ~TcpServer() noexcept;

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    TcpServer(TcpServer&&) = delete;
    TcpServer& operator=(TcpServer&&) = delete;

    // Public operations may overlap one another, including shutdown() and
    // enqueue(). The TcpServer object must outlive every public call; destruction
    // must not overlap an external call.
    void start();
    void shutdown() noexcept;
    // The first unexpected Winsock shutdown error retained from any session.
    [[nodiscard]] std::error_code shutdown_error() const noexcept;
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::uint16_t bound_port() const;
    [[nodiscard]] std::size_t active_connection_count() const noexcept;
    [[nodiscard]] std::vector<ConnectionId> connection_ids() const;
    // Blocks while the per-connection queue is full; use stop_token to cancel the wait.
    // Nonblocking admission; a full per-connection queue requests closure of that connection.
    [[nodiscard]] EnqueueStatus try_enqueue(ConnectionId connection_id, std::vector<std::byte> bytes);
    [[nodiscard]] EnqueueStatus enqueue(
        ConnectionId connection_id,
        std::vector<std::byte> bytes,
        std::stop_token stop_token = {});

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace telemetryhub::network