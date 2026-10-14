#include <telemetryhub/server/command_ingress.hpp>

#include <telemetryhub/network/tcp.hpp>
#include <telemetryhub/protocol/message_codec.hpp>
#include <telemetryhub/protocol/message_framer.hpp>
#include <telemetryhub/server/processing_response.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace telemetryhub::server {
namespace {

[[nodiscard]] std::optional<std::vector<std::byte>> encode_response(
    const protocol::Response& response)
{
    auto payload = protocol::serialize_response_payload(response);
    if (payload.error.has_value())
    {
        return std::nullopt;
    }
    auto frame = protocol::encode_frame(payload.payload);
    if (frame.error.has_value())
    {
        return std::nullopt;
    }
    return std::move(frame.frame);
}

} // namespace

CommandIngress::CommandIngress(
    domain::TelemetryProcessor& processor,
    const std::size_t capacity)
    : processor_{processor},
      commands_{capacity}
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

    const auto sequence = allocate_receive_sequence();
    if (!sequence.has_value())
    {
        return CommandAdmission::sequence_exhausted;
    }
    envelope.receive_sequence = *sequence;

    // try_push is the queue-close linearization point. The allocated sequence
    // may be unused when this attempt loses to saturation or shutdown.
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

std::optional<domain::ReceiveSequence> CommandIngress::allocate_receive_sequence() noexcept
{
    using Sequence = domain::ReceiveSequence;
    Sequence previous = last_receive_sequence_.load(std::memory_order_relaxed);
    while (previous != (std::numeric_limits<Sequence>::max)())
    {
        const Sequence next = previous + 1U;
        if (last_receive_sequence_.compare_exchange_weak(
                previous, next, std::memory_order_relaxed, std::memory_order_relaxed))
        {
            return next;
        }
    }
    return std::nullopt;
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
            process_and_respond(*item.value, response_sink);
        }
    }
    catch (...)
    {
        // No exception may escape the owned consumer thread.
    }
}

void CommandIngress::process_and_respond(
    const CommandEnvelope& envelope,
    const ResponseSink& response_sink) noexcept
{
    bool sink_invoked = false;
    try
    {
        const protocol::Response response = make_processing_response(envelope, processor_);
        auto frame = encode_response(response);
        if (!frame.has_value())
        {
            attempt_fallback_response(envelope, response_sink, sink_invoked);
            return;
        }

        // Once invocation begins, the sink may have accepted the frame even if
        // it returns an error or throws. Never retry this request after this point.
        sink_invoked = true;
        static_cast<void>(response_sink(envelope.connection_id, std::move(*frame)));
    }
    catch (...)
    {
        if (!sink_invoked)
        {
            attempt_fallback_response(envelope, response_sink, sink_invoked);
        }
    }
}

void CommandIngress::attempt_fallback_response(
    const CommandEnvelope& envelope,
    const ResponseSink& response_sink,
    bool& sink_invoked) noexcept
{
    try
    {
        const protocol::Response response =
            make_processing_unavailable_response(envelope.request_id);
        auto frame = encode_response(response);
        if (!frame.has_value())
        {
            return;
        }

        sink_invoked = true;
        static_cast<void>(response_sink(envelope.connection_id, std::move(*frame)));
    }
    catch (...)
    {
        // A failed fallback cannot stop draining later accepted commands.
    }
}

} // namespace telemetryhub::server
