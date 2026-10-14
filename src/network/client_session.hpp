#pragma once

#include <telemetryhub/concurrency/bounded_blocking_queue.hpp>
#include <telemetryhub/network/socket.hpp>
#include <telemetryhub/network/tcp_server.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <system_error>
#include <thread>
#include <vector>

namespace telemetryhub::network {

class ClientSession
{
public:
    ClientSession(
        ConnectionId id,
        Socket socket,
        std::size_t outbound_capacity,
        std::size_t max_outbound_buffer_size_bytes,
        std::chrono::milliseconds terminal_flush_timeout,
        SessionCallbacks callbacks);
    ~ClientSession() noexcept;

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    void start();
    void request_stop() noexcept;
    void join() noexcept;
    [[nodiscard]] bool completed() const noexcept;
    [[nodiscard]] std::error_code shutdown_error() const noexcept;
    [[nodiscard]] ConnectionId id() const noexcept;
    [[nodiscard]] EnqueueStatus try_enqueue(std::vector<std::byte> bytes);
    [[nodiscard]] EnqueueStatus enqueue(
        std::vector<std::byte> bytes,
        std::stop_token stop_token);

private:
    void reader_loop(std::stop_token stop_token) noexcept;
    void writer_loop(std::stop_token stop_token) noexcept;
    void thread_finished() noexcept;
    void notify_disconnected() noexcept;
    [[nodiscard]] bool queue_output(SessionOutput output, std::stop_token stop_token);

    ConnectionId id_;
    Socket socket_;
    const std::size_t max_outbound_buffer_size_bytes_;
    const std::chrono::milliseconds terminal_flush_timeout_;
    SessionCallbacks callbacks_;
    concurrency::BoundedBlockingQueue<std::vector<std::byte>> outbound_;
    std::stop_source stop_source_;
    std::jthread reader_;
    std::jthread writer_;
    std::atomic<unsigned int> remaining_threads_{0U};
    std::atomic<bool> completed_{false};
    std::atomic<bool> disconnected_notified_{false};
    std::atomic<int> shutdown_error_code_{0};

    std::atomic<std::int64_t> flush_deadline_ticks_{0};
};

} // namespace telemetryhub::network
