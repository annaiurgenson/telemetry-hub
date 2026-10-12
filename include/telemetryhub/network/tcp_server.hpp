#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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

struct TcpServerConfig
{
    std::uint16_t port{0U};
    std::size_t max_connections{default_max_active_connections};
    std::size_t outbound_queue_capacity{default_outbound_queue_capacity};
    std::size_t max_outbound_buffer_size_bytes{default_max_outbound_buffer_size_bytes};
};

enum class EnqueueStatus
{
    accepted,
    unknown_connection,
    too_large,
    closed,
    cancelled
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
    [[nodiscard]] EnqueueStatus enqueue(
        ConnectionId connection_id,
        std::vector<std::byte> bytes,
        std::stop_token stop_token = {});

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace telemetryhub::network