#include "client_session.hpp"

#include <telemetryhub/network/tcp.hpp>
#include <telemetryhub/network/winsock_error.hpp>

#include <array>
#include <utility>

namespace telemetryhub::network {
namespace {

constexpr std::size_t receive_buffer_size = 4096U;

} // namespace

ClientSession::ClientSession(
    const ConnectionId id,
    Socket socket,
    const std::size_t outbound_capacity,
    const std::size_t max_outbound_buffer_size_bytes)
    : id_{id},
      socket_{std::move(socket)},
      max_outbound_buffer_size_bytes_{max_outbound_buffer_size_bytes},
      outbound_{outbound_capacity}
{
}

ClientSession::~ClientSession() noexcept
{
    request_stop();
    join();
}

void ClientSession::start()
{
    remaining_threads_.store(2U);
    try
    {
        const std::stop_token token = stop_source_.get_token();
        reader_ = std::jthread{[this, token] { reader_loop(token); }};
        writer_ = std::jthread{[this, token] { writer_loop(token); }};
    }
    catch (...)
    {
        request_stop();
        if (reader_.joinable())
        {
            reader_.join();
        }
        remaining_threads_.store(0U);
        completed_.store(true);
        throw;
    }
}

void ClientSession::request_stop() noexcept
{
    if (!stop_source_.request_stop())
    {
        return;
    }

    outbound_.close();
    const std::error_code error = socket_.shutdown(SD_BOTH);
    if (error && error.value() != WSAENOTCONN && error.value() != WSAESHUTDOWN &&
        error.value() != WSAECONNRESET && error.value() != WSAECONNABORTED &&
        error.value() != WSAENETRESET)
    {
        shutdown_error_code_.store(error.value());
    }
}

void ClientSession::join() noexcept
{
    if (reader_.joinable())
    {
        reader_.join();
    }
    if (writer_.joinable())
    {
        writer_.join();
    }
}

bool ClientSession::completed() const noexcept
{
    return completed_.load();
}

std::error_code ClientSession::shutdown_error() const noexcept
{
    const int error = shutdown_error_code_.load();
    return error == 0 ? std::error_code{} : make_winsock_error_code(error);
}

ConnectionId ClientSession::id() const noexcept
{
    return id_;
}

EnqueueStatus ClientSession::enqueue(
    std::vector<std::byte> bytes,
    const std::stop_token stop_token)
{
    if (bytes.size() > max_outbound_buffer_size_bytes_)
    {
        return EnqueueStatus::too_large;
    }
    if (stop_source_.stop_requested())
    {
        return EnqueueStatus::closed;
    }
    const auto status = outbound_.push(std::move(bytes), stop_token);
    switch (status)
    {
    case concurrency::QueueOperationStatus::success:
        return EnqueueStatus::accepted;
    case concurrency::QueueOperationStatus::cancelled:
        return EnqueueStatus::cancelled;
    case concurrency::QueueOperationStatus::closed:
        return EnqueueStatus::closed;
    }
    return EnqueueStatus::closed;
}

void ClientSession::reader_loop(const std::stop_token stop_token) noexcept
{
    try
    {
        std::array<std::byte, receive_buffer_size> buffer{};
        while (!stop_token.stop_requested())
        {
            const ReceiveResult result = receive_some(socket_, buffer, stop_token);
            if (result.status != ReceiveStatus::data)
            {
                break;
            }
            // Milestone 4A deliberately discards application bytes until 4B adds protocol processing.
        }
    }
    catch (...)
    {
    }
    request_stop();
    thread_finished();
}

void ClientSession::writer_loop(const std::stop_token stop_token) noexcept
{
    try
    {
        while (!stop_token.stop_requested())
        {
            auto item = outbound_.pop(stop_token);
            if (item.status != concurrency::QueueOperationStatus::success || !item.value)
            {
                break;
            }

            const SendResult result = send_all(socket_, *item.value, stop_token);
            if (result.error || result.cancelled)
            {
                break;
            }
        }
    }
    catch (...)
    {
    }
    request_stop();
    thread_finished();
}

void ClientSession::thread_finished() noexcept
{
    if (remaining_threads_.fetch_sub(1U) == 1U)
    {
        completed_.store(true);
    }
}

} // namespace telemetryhub::network