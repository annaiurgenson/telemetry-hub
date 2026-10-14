#pragma once

#include <telemetryhub/concurrency/bounded_blocking_queue.hpp>
#include <telemetryhub/concurrency/worker_pool.hpp>
#include <telemetryhub/domain/telemetry_processor.hpp>
#include <telemetryhub/server/command_envelope.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace telemetryhub::server {

inline constexpr std::size_t default_incoming_command_queue_capacity = 1'024U;
inline constexpr std::size_t default_telemetry_worker_count = 4U;
inline constexpr std::size_t default_worker_pool_task_queue_capacity = 1'024U;

enum class CommandAdmission
{
    accepted,
    full,
    closed,
    sequence_exhausted
};

class CommandIngress
{
public:
    using ResponseSink = std::function<network::EnqueueStatus(
        network::ConnectionId, std::vector<std::byte>)>;

    explicit CommandIngress(
        domain::TelemetryProcessor& processor,
        concurrency::WorkerPool& worker_pool,
        std::size_t capacity = default_incoming_command_queue_capacity);
    ~CommandIngress() noexcept;

    CommandIngress(const CommandIngress&) = delete;
    CommandIngress& operator=(const CommandIngress&) = delete;

    // Concurrent submit() and shutdown() are supported. The object must outlive
    // all public calls; destruction must not overlap an external call.
    // Worker tasks own their command and sink. The sink may be invoked
    // concurrently, must return promptly, and must not call shutdown() or
    // destroy this object. Start once before enabling callbacks.
    void start(ResponseSink response_sink);
    [[nodiscard]] CommandAdmission submit(CommandEnvelope envelope);
    // Closes admission, dispatches accepted commands, and joins the consumer.
    // The borrowed WorkerPool must remain active until shutdown() returns.
    void shutdown() noexcept;

private:
    [[nodiscard]] std::optional<domain::ReceiveSequence> allocate_receive_sequence() noexcept;
    void consume(const ResponseSink& response_sink) noexcept;
    void dispatch(
        CommandEnvelope envelope,
        const ResponseSink& response_sink) noexcept;
    static void process_and_respond(
        const CommandEnvelope& envelope,
        domain::TelemetryProcessor& processor,
        const ResponseSink& response_sink) noexcept;
    static void attempt_fallback_response(
        const CommandEnvelope& envelope,
        const ResponseSink& response_sink,
        bool& sink_invoked) noexcept;

    domain::TelemetryProcessor& processor_;
    concurrency::WorkerPool& worker_pool_;
    concurrency::BoundedBlockingQueue<CommandEnvelope> commands_;
    std::atomic<domain::ReceiveSequence> last_receive_sequence_{0U};
    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_changed_;
    std::jthread consumer_;
    bool started_{false};
    bool stopping_{false};
    bool stopped_{false};
};

} // namespace telemetryhub::server
