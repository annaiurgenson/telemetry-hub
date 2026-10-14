#include <telemetryhub/server/command_ingress.hpp>

#include <telemetryhub/network/tcp.hpp>
#include <telemetryhub/protocol/message_codec.hpp>
#include <telemetryhub/protocol/message_framer.hpp>

#include <stdexcept>
#include <utility>

namespace telemetryhub::server {
namespace {

[[nodiscard]] std::vector<std::byte> unavailable_frame(const CommandEnvelope& envelope)
{
    const protocol::Response response{
        envelope.request_id,
        protocol::ResponseBody{protocol::ErrorResponse{
            "PROCESSING_UNAVAILABLE", "Command processing is not available yet"}}};
    auto payload = protocol::serialize_response_payload(response);
    if (payload.error.has_value())
    {
        return {};
    }
    auto frame = protocol::encode_frame(payload.payload);
    if (frame.error.has_value())
    {
        return {};
    }
    return std::move(frame.frame);
}

} // namespace

CommandIngress::CommandIngress(const std::size_t capacity)
    : commands_{capacity}
{
}

CommandIngress::~CommandIngress() noexcept
{
    shutdown();
}

void CommandIngress::start(ResponseSink response_sink)
{
    if (!response_sink)
    {
        throw std::invalid_argument{"CommandIngress requires a response sink"};
    }

    std::lock_guard lock{lifecycle_mutex_};
    if (started_ || stopping_ || stopped_)
    {
        throw std::logic_error{"CommandIngress can only be started once before shutdown"};
    }

    consumer_ = std::jthread{
        [this, sink = std::move(response_sink)](const std::stop_token) noexcept {
            consume(sink);
        }};
    started_ = true;
}

CommandAdmission CommandIngress::submit(CommandEnvelope envelope)
{
    {
        std::lock_guard lock{lifecycle_mutex_};
        if (!started_ || stopped_)
        {
            return CommandAdmission::closed;
        }
    }

    switch (commands_.try_push(std::move(envelope)))
    {
    case concurrency::QueueOperationStatus::success:
        return CommandAdmission::accepted;
    case concurrency::QueueOperationStatus::full:
        return CommandAdmission::full;
    case concurrency::QueueOperationStatus::closed:
    case concurrency::QueueOperationStatus::cancelled:
        return CommandAdmission::closed;
    }
    return CommandAdmission::closed;
}

void CommandIngress::shutdown() noexcept
{
    {
        std::unique_lock lock{lifecycle_mutex_};
        if (stopping_)
        {
            lifecycle_changed_.wait(lock, [this] { return !stopping_; });
            return;
        }
        if (stopped_)
        {
            return;
        }
        stopping_ = true;
    }

    // Queue closure is the admission boundary. A concurrent submit either
    // linearizes before close and is drained, or observes closed and is rejected.
    commands_.close();
    if (consumer_.joinable())
    {
        consumer_.join();
    }

    {
        std::lock_guard lock{lifecycle_mutex_};
        stopped_ = true;
        stopping_ = false;
    }
    lifecycle_changed_.notify_all();
}

void CommandIngress::consume(const ResponseSink& response_sink) noexcept
{
    try
    {
        while (true)
        {
            auto item = commands_.pop();
            if (item.status != concurrency::QueueOperationStatus::success || !item.value)
            {
                break;
            }
            respond_unavailable(*item.value, response_sink);
        }
    }
    catch (...)
    {
        // No exception may escape the owned consumer thread.
    }
}

void CommandIngress::respond_unavailable(
    const CommandEnvelope& envelope,
    const ResponseSink& response_sink) noexcept
{
    try
    {
        std::vector<std::byte> frame = unavailable_frame(envelope);
        if (!frame.empty())
        {
            // try_enqueue is nonblocking. Full/closed/unknown outcomes are
            // local to this connection and cannot stall the consumer.
            static_cast<void>(response_sink(envelope.connection_id, std::move(frame)));
        }
    }
    catch (...)
    {
        // A failed response affects only this envelope; keep draining others.
    }
}

} // namespace telemetryhub::server
