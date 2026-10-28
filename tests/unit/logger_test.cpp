#include <telemetryhub/support/logger.hpp>

#include "support/logger_internal.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <ostream>
#include <semaphore>
#include <sstream>
#include <streambuf>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace telemetryhub::support {
namespace {

using namespace std::chrono;
using namespace std::chrono_literals;
using nlohmann::json;

class ControlledBuffer final : public std::streambuf
{
public:
    explicit ControlledBuffer(
        const std::size_t accepted_bytes = std::numeric_limits<std::size_t>::max(),
        const bool fail_sync = false)
        : accepted_bytes_{accepted_bytes},
          fail_sync_{fail_sync}
    {
    }

    [[nodiscard]] std::string contents() const
    {
        return contents_;
    }

    [[nodiscard]] std::size_t sync_count() const noexcept
    {
        return sync_count_;
    }

protected:
    std::streamsize xsputn(const char* source, const std::streamsize count) override
    {
        const auto accepted = std::min<std::size_t>(
            static_cast<std::size_t>(count), accepted_bytes_);
        contents_.append(source, accepted);
        accepted_bytes_ -= accepted;
        return static_cast<std::streamsize>(accepted);
    }

    int_type overflow(const int_type character) override
    {
        if (traits_type::eq_int_type(character, traits_type::eof()) || accepted_bytes_ == 0U)
        {
            return traits_type::eof();
        }
        contents_.push_back(traits_type::to_char_type(character));
        --accepted_bytes_;
        return character;
    }

private:
    int sync() override
    {
        ++sync_count_;
        return fail_sync_ ? -1 : 0;
    }

    std::size_t accepted_bytes_;
    bool fail_sync_;
    std::size_t sync_count_{};
    std::string contents_;
};

class ThrowingBuffer final : public std::streambuf
{
protected:
    std::streamsize xsputn(const char*, const std::streamsize) override
    {
        throw std::runtime_error{"controlled stream buffer failure"};
    }
};

class BlockingFailBuffer final : public std::streambuf
{
public:
    BlockingFailBuffer(std::binary_semaphore& entered, std::binary_semaphore& release)
        : entered_{entered},
          release_{release}
    {
    }

protected:
    std::streamsize xsputn(const char*, const std::streamsize) override
    {
        entered_.release();
        release_.acquire();
        return 0;
    }

private:
    std::binary_semaphore& entered_;
    std::binary_semaphore& release_;
};

[[nodiscard]] std::string only_line(const std::string& text)
{
    const std::size_t newline = text.find('\n');
    EXPECT_NE(newline, std::string::npos);
    EXPECT_EQ(text.find('\n', newline == std::string::npos ? 0U : newline + 1U), std::string::npos);
    return text.substr(0U, newline);
}

[[nodiscard]] system_clock::time_point utc_point(
    const year_month_day date,
    const hours hour = 0h,
    const minutes minute = 0min,
    const seconds second = 0s,
    const milliseconds millisecond = 0ms)
{
    const auto days_since_epoch = static_cast<std::int64_t>(sys_days{date}.time_since_epoch().count());
    const std::int64_t milliseconds_since_epoch =
        days_since_epoch * 86'400'000LL +
        static_cast<std::int64_t>(hour.count()) * 3'600'000LL +
        static_cast<std::int64_t>(minute.count()) * 60'000LL +
        static_cast<std::int64_t>(second.count()) * 1'000LL +
        static_cast<std::int64_t>(millisecond.count());
    return system_clock::time_point{
        duration_cast<system_clock::duration>(milliseconds{milliseconds_since_epoch})};
}

TEST(Logger, WritesStableJsonLinesSchemaAndSupportedValues)
{
    std::ostringstream output;
    Logger logger{output, LogLevel::debug};
    logger.log(
        LogLevel::warning,
        LogEvent::accept_failed,
        {{LogFieldKey::port, std::uint64_t{42U}},
         {LogFieldKey::error_code, std::int64_t{-7}},
         {LogFieldKey::request_id, std::uint64_t{9'007'199'254'740'993ULL}},
         {LogFieldKey::connection_id, true}});

    const std::string line = only_line(output.str());
    EXPECT_EQ(line.find("\"timestamp\":"), 1U);
    EXPECT_LT(line.find("\"level\":"), line.find("\"thread_id\":"));
    EXPECT_LT(line.find("\"thread_id\":"), line.find("\"event\":"));
    EXPECT_LT(line.find("\"event\":"), line.find("\"fields\":"));

    const json record = json::parse(line);
    EXPECT_EQ(record.size(), 5U);
    EXPECT_EQ(record.at("level"), "warning");
    EXPECT_EQ(record.at("event"), "accept_failed");
    EXPECT_TRUE(record.at("timestamp").get<std::string>().ends_with('Z'));
    EXPECT_TRUE(record.at("thread_id").is_string());
    EXPECT_EQ(record.at("fields").at("port"), 42U);
    EXPECT_EQ(record.at("fields").at("error_code"), -7);
    EXPECT_EQ(record.at("fields").at("request_id"), 9'007'199'254'740'993ULL);
    EXPECT_EQ(record.at("fields").at("connection_id"), true);
    EXPECT_FALSE(logger.output_failed());
}

TEST(Logger, FlushesEachSuccessfullyWrittenRecord)
{
    ControlledBuffer buffer;
    std::ostream output{&buffer};
    Logger logger{output, LogLevel::debug};
    logger.log(LogLevel::info, LogEvent::application_started);
    logger.log(LogLevel::info, LogEvent::application_stopped);

    EXPECT_EQ(buffer.sync_count(), 2U);
    EXPECT_FALSE(logger.output_failed());
}

TEST(Logger, EmitsStableLevelAndEventNamesAndEmptyFields)
{
    constexpr LogLevel levels[]{LogLevel::debug, LogLevel::info, LogLevel::warning, LogLevel::error};
    constexpr const char* level_names[]{"debug", "info", "warning", "error"};
    constexpr LogEvent events[]{
        LogEvent::application_started,
        LogEvent::application_stopping,
        LogEvent::application_stopped,
        LogEvent::application_start_failed,
        LogEvent::accept_failed,
        LogEvent::session_receive_failed,
        LogEvent::session_send_failed,
        LogEvent::command_dispatcher_failed,
        LogEvent::command_processing_failed,
        LogEvent::unknown_failure};
    constexpr const char* event_names[]{
        "application_started", "application_stopping", "application_stopped",
        "application_start_failed", "accept_failed", "session_receive_failed",
        "session_send_failed", "command_dispatcher_failed", "command_processing_failed",
        "unknown_failure"};

    for (std::size_t index = 0U; index < std::size(levels); ++index)
    {
        std::ostringstream output;
        Logger logger{output, LogLevel::debug};
        logger.log(levels[index], LogEvent::application_started);
        const json record = json::parse(only_line(output.str()));
        EXPECT_EQ(record.at("level"), level_names[index]);
        EXPECT_EQ(record.at("fields"), json::object());
    }

    for (std::size_t index = 0U; index < std::size(events); ++index)
    {
        std::ostringstream output;
        Logger logger{output, LogLevel::debug};
        logger.log(LogLevel::error, events[index]);
        EXPECT_EQ(json::parse(only_line(output.str())).at("event"), event_names[index]);
    }
}

TEST(Logger, AppliesMinimumSeverityAndRejectsInvalidInputsWithoutDisablingOutput)
{
    std::ostringstream output;
    Logger logger{output, LogLevel::warning};
    logger.log(LogLevel::info, LogEvent::application_started);
    EXPECT_TRUE(output.str().empty());

    logger.log(static_cast<LogLevel>(99), LogEvent::application_started);
    logger.log(LogLevel::error, static_cast<LogEvent>(99));
    logger.log(LogLevel::error, LogEvent::application_started,
               {{static_cast<LogFieldKey>(99), std::int64_t{1}}});
    logger.log(LogLevel::error, LogEvent::application_started,
               {{LogFieldKey::port, std::uint64_t{1U}},
                {LogFieldKey::port, std::uint64_t{2U}}});
    logger.log(LogLevel::error, LogEvent::application_started,
               {{LogFieldKey::port, std::uint64_t{1U}},
                {LogFieldKey::connection_id, std::uint64_t{2U}},
                {LogFieldKey::request_id, std::uint64_t{3U}},
                {LogFieldKey::error_code, std::int64_t{4}},
                {static_cast<LogFieldKey>(99), std::int64_t{5}}});

    EXPECT_TRUE(output.str().empty());
    logger.log(LogLevel::warning, LogEvent::application_started);
    const std::string output_text = output.str();
    EXPECT_EQ(std::count(output_text.begin(), output_text.end(), '\n'), 1);
    EXPECT_FALSE(logger.output_failed());
}

TEST(Logger, EnforcesThreadIdAndRecordSizeLimitsAndRecovers)
{
    using namespace telemetryhub::support::detail;
    const auto timestamp = utc_point(year{2026}/October/28);
    EXPECT_FALSE(format_record(
        LogLevel::info, LogEvent::application_started, {}, timestamp,
        std::string(max_thread_id_size_bytes + 1U, 'a')).has_value());
    EXPECT_FALSE(format_record(
        LogLevel::info, LogEvent::application_started, {}, timestamp,
        std::string(max_thread_id_size_bytes, '\n')).has_value());

    EXPECT_FALSE(format_utc_timestamp(system_clock::time_point::max()).has_value());
    std::ostringstream output;
    Logger logger{output, LogLevel::debug};
    logger.log(LogLevel::info, LogEvent::application_started);
    EXPECT_EQ(json::parse(only_line(output.str())).at("event"), "application_started");
    EXPECT_FALSE(logger.output_failed());
}

TEST(Logger, FormatsUtcTimestampsExactlyAtEpochAndCalendarBoundaries)
{
    using namespace telemetryhub::support::detail;
    const auto format = [](const system_clock::time_point point) {
        const auto result = format_utc_timestamp(point);
        EXPECT_TRUE(result.has_value());
        return result.value_or(std::string{});
    };

    EXPECT_EQ(format(system_clock::time_point{}), "1970-01-01T00:00:00.000Z");
    EXPECT_EQ(format(system_clock::time_point{} - 1ms), "1969-12-31T23:59:59.999Z");
    EXPECT_EQ(
        format(utc_point(year{2024}/February/29, 23h, 59min, 59s, 999ms)),
        "2024-02-29T23:59:59.999Z");
    EXPECT_EQ(
        format(utc_point(year{2024}/February/29, 23h, 59min, 59s, 999ms) + 1ms),
        "2024-03-01T00:00:00.000Z");
    EXPECT_EQ(
        format(utc_point(year{2024}/April/30, 23h, 59min, 59s, 999ms) + 1ms),
        "2024-05-01T00:00:00.000Z");
    EXPECT_EQ(
        format(utc_point(year{2024}/December/31, 23h, 59min, 59s, 999ms) + 1ms),
        "2025-01-01T00:00:00.000Z");
    EXPECT_EQ(
        format(utc_point(year{2024}/January/1) + 1'234'567us),
        "2024-01-01T00:00:01.234Z");
    EXPECT_EQ(format(utc_point(year{0}/January/1)), "0000-01-01T00:00:00.000Z");
    EXPECT_EQ(
        format(utc_point(year{9999}/December/31, 23h, 59min, 59s, 999ms)),
        "9999-12-31T23:59:59.999Z");
    EXPECT_FALSE(format_utc_timestamp(utc_point(year{-1}/January/1)).has_value());
    EXPECT_FALSE(format_utc_timestamp(
        utc_point(year{9999}/December/31, 23h, 59min, 59s, 999ms) + 1ms).has_value());
}

TEST(Logger, SerializesConcurrentRecordsWithoutLossOrInterleaving)
{
    constexpr std::size_t thread_count = 8U;
    constexpr std::size_t records_per_thread = 50U;
    std::ostringstream output;
    Logger logger{output, LogLevel::debug};
    std::barrier start{static_cast<std::ptrdiff_t>(thread_count + 1U)};
    std::vector<std::jthread> threads;
    threads.reserve(thread_count);
    for (std::size_t thread_index = 0U; thread_index < thread_count; ++thread_index)
    {
        threads.emplace_back([&, thread_index] {
            start.arrive_and_wait();
            for (std::size_t record_index = 0U; record_index < records_per_thread; ++record_index)
            {
                const auto request_id = static_cast<std::uint64_t>(
                    thread_index * records_per_thread + record_index);
                logger.log(
                    LogLevel::info,
                    LogEvent::command_processing_failed,
                    {{LogFieldKey::request_id, request_id}});
            }
        });
    }
    start.arrive_and_wait();
    threads.clear();

    std::istringstream lines{output.str()};
    std::string line;
    std::vector<bool> seen(thread_count * records_per_thread, false);
    std::size_t line_count = 0U;
    while (std::getline(lines, line))
    {
        const json record = json::parse(line);
        EXPECT_EQ(record.at("event"), "command_processing_failed");
        const auto request_id = record.at("fields").at("request_id").get<std::uint64_t>();
        ASSERT_LT(request_id, seen.size());
        EXPECT_FALSE(seen[static_cast<std::size_t>(request_id)]);
        seen[static_cast<std::size_t>(request_id)] = true;
        ++line_count;
    }
    EXPECT_EQ(line_count, thread_count * records_per_thread);
    EXPECT_TRUE(std::all_of(seen.begin(), seen.end(), [](const bool value) { return value; }));
    EXPECT_FALSE(logger.output_failed());
}

TEST(Logger, DetectsNonThrowingStreamStateFailureAndDisablesFurtherOutput)
{
    ControlledBuffer buffer{0U};
    std::ostream output{&buffer};
    Logger logger{output, LogLevel::debug};

    logger.log(LogLevel::error, LogEvent::unknown_failure);
    EXPECT_TRUE(logger.output_failed());
    const std::string before = buffer.contents();
    logger.log(LogLevel::error, LogEvent::unknown_failure);
    EXPECT_EQ(buffer.contents(), before);

    ControlledBuffer flush_buffer{std::numeric_limits<std::size_t>::max(), true};
    std::ostream flush_output{&flush_buffer};
    Logger flush_logger{flush_output, LogLevel::debug};
    flush_logger.log(LogLevel::error, LogEvent::unknown_failure);
    EXPECT_TRUE(flush_logger.output_failed());
}

TEST(Logger, DetectsThrowingAndPartialWriteFailuresWithoutRetry)
{
    ThrowingBuffer throwing_buffer;
    std::ostream throwing_output{&throwing_buffer};
    throwing_output.exceptions(std::ios::badbit);
    Logger throwing_logger{throwing_output, LogLevel::debug};
    throwing_logger.log(LogLevel::error, LogEvent::unknown_failure);
    EXPECT_TRUE(throwing_logger.output_failed());

    ControlledBuffer partial_buffer{8U};
    std::ostream partial_output{&partial_buffer};
    Logger partial_logger{partial_output, LogLevel::debug};
    partial_logger.log(LogLevel::error, LogEvent::unknown_failure);
    EXPECT_TRUE(partial_logger.output_failed());
    const std::string partial = partial_buffer.contents();
    EXPECT_EQ(partial.size(), 8U);
    partial_logger.log(LogLevel::error, LogEvent::unknown_failure);
    EXPECT_EQ(partial_buffer.contents(), partial);
}

TEST(Logger, OutputFailureCanBeObservedConcurrently)
{
    constexpr auto wait_timeout = 2s;
    std::binary_semaphore write_entered{0};
    std::binary_semaphore allow_write_failure{0};
    BlockingFailBuffer buffer{write_entered, allow_write_failure};
    std::ostream output{&buffer};
    Logger logger{output, LogLevel::debug};
    std::atomic<bool> observed_failure{false};
    std::atomic<bool> stop_observer{false};
    std::binary_semaphore observer_started{0};
    std::binary_semaphore observer_done{0};

    std::jthread writer{[&] { logger.log(LogLevel::error, LogEvent::unknown_failure); }};
    const bool write_was_entered = write_entered.try_acquire_for(wait_timeout);
    if (!write_was_entered)
    {
        allow_write_failure.release();
        writer.join();
        ADD_FAILURE() << "Logger did not enter the controlled stream write";
        return;
    }

    EXPECT_FALSE(logger.output_failed());
    std::jthread observer{[&] {
        observer_started.release();
        const auto deadline = steady_clock::now() + wait_timeout;
        while (!logger.output_failed() &&
               !stop_observer.load(std::memory_order_acquire) &&
               steady_clock::now() < deadline)
        {
            std::this_thread::yield();
        }
        observed_failure.store(logger.output_failed(), std::memory_order_release);
        observer_done.release();
    }};
    const bool observer_was_started = observer_started.try_acquire_for(wait_timeout);
    if (!observer_was_started)
    {
        stop_observer.store(true, std::memory_order_release);
    }

    allow_write_failure.release();
    writer.join();
    const bool observer_finished = observer_done.try_acquire_for(wait_timeout);
    if (!observer_finished)
    {
        stop_observer.store(true, std::memory_order_release);
        static_cast<void>(observer_done.try_acquire_for(wait_timeout));
    }
    observer.join();

    EXPECT_TRUE(observer_was_started);
    EXPECT_TRUE(observer_finished);
    EXPECT_TRUE(observed_failure.load(std::memory_order_acquire));
}

} // namespace
} // namespace telemetryhub::support
