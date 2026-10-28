#pragma once

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <iosfwd>
#include <mutex>
#include <variant>

namespace telemetryhub::support {

enum class LogLevel
{
    debug,
    info,
    warning,
    error
};

enum class LogEvent
{
    application_started,
    application_stopping,
    application_stopped,
    application_start_failed,
    accept_failed,
    session_receive_failed,
    session_send_failed,
    command_dispatcher_failed,
    command_processing_failed,
    unknown_failure
};

enum class LogFieldKey
{
    port,
    connection_id,
    request_id,
    error_code
};

using LogValue = std::variant<std::int64_t, std::uint64_t, bool>;

struct LogField
{
    LogFieldKey key;
    LogValue value;
};

class Logger
{
public:
    // Borrows output; it must outlive this Logger. The stream must not re-enter
    // this Logger, and destruction must not overlap any public operation,
    // including log() and output_failed().
    explicit Logger(std::ostream& output, LogLevel minimum = LogLevel::info);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    // Fields are formatted during this call and are never retained. Output is
    // synchronous and serialized, and may block. Formatting failures drop one
    // record; output failures may leave a partial record and disable later
    // output. Argument expressions are evaluated before entry and are outside
    // this noexcept guarantee.
    void log(
        LogLevel level,
        LogEvent event,
        std::initializer_list<LogField> fields = {}) noexcept;

    [[nodiscard]] bool output_failed() const noexcept;

private:
    std::ostream& output_;
    const LogLevel minimum_;
    std::mutex output_mutex_;
    std::atomic<bool> output_failed_{false};
};

} // namespace telemetryhub::support
