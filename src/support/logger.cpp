#include <telemetryhub/support/logger.hpp>

#include "support/logger_internal.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace telemetryhub::support {
using namespace std::string_view_literals;

namespace {

[[nodiscard]] int severity_rank(const LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::debug:
        return 0;
    case LogLevel::info:
        return 1;
    case LogLevel::warning:
        return 2;
    case LogLevel::error:
        return 3;
    }
    return -1;
}

[[nodiscard]] std::string_view level_name(const LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::debug:
        return "debug";
    case LogLevel::info:
        return "info";
    case LogLevel::warning:
        return "warning";
    case LogLevel::error:
        return "error";
    }
    return {};
}

[[nodiscard]] std::string_view event_name(const LogEvent event) noexcept
{
    switch (event)
    {
    case LogEvent::application_started:
        return "application_started";
    case LogEvent::application_stopping:
        return "application_stopping";
    case LogEvent::application_stopped:
        return "application_stopped";
    case LogEvent::application_start_failed:
        return "application_start_failed";
    case LogEvent::accept_failed:
        return "accept_failed";
    case LogEvent::session_receive_failed:
        return "session_receive_failed";
    case LogEvent::session_send_failed:
        return "session_send_failed";
    case LogEvent::command_dispatcher_failed:
        return "command_dispatcher_failed";
    case LogEvent::command_processing_failed:
        return "command_processing_failed";
    case LogEvent::unknown_failure:
        return "unknown_failure";
    }
    return {};
}

[[nodiscard]] std::string_view field_name(const LogFieldKey key) noexcept
{
    switch (key)
    {
    case LogFieldKey::port:
        return "port";
    case LogFieldKey::connection_id:
        return "connection_id";
    case LogFieldKey::request_id:
        return "request_id";
    case LogFieldKey::error_code:
        return "error_code";
    }
    return {};
}

[[nodiscard]] bool append_bounded(std::string& output, const std::string_view text)
{
    if (output.size() > detail::max_log_record_size_bytes ||
        text.size() > detail::max_log_record_size_bytes - output.size())
    {
        return false;
    }
    output.append(text);
    return true;
}

[[nodiscard]] bool append_json_string(std::string& output, const std::string_view text)
{
    constexpr char hex_digits[] = "0123456789abcdef";
    if (!append_bounded(output, "\""))
    {
        return false;
    }

    for (const unsigned char byte : text)
    {
        if (byte == static_cast<unsigned char>('"') || byte == static_cast<unsigned char>('\\'))
        {
            if (!append_bounded(output, "\\"))
            {
                return false;
            }
            const char escaped = static_cast<char>(byte);
            if (!append_bounded(output, std::string_view{&escaped, 1U}))
            {
                return false;
            }
        }
        else if (byte < 0x20U || byte >= 0x7fU)
        {
            const char escaped[] = {
                '\\', 'u', '0', '0', hex_digits[(byte >> 4U) & 0x0fU],
                hex_digits[byte & 0x0fU]};
            if (!append_bounded(output, std::string_view{escaped, sizeof(escaped)}))
            {
                return false;
            }
        }
        else
        {
            const char character = static_cast<char>(byte);
            if (!append_bounded(output, std::string_view{&character, 1U}))
            {
                return false;
            }
        }
    }

    return append_bounded(output, "\"");
}

template <typename Integer>
[[nodiscard]] bool append_integer(std::string& output, const Integer value)
{
    std::array<char, 32U> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{})
    {
        return false;
    }
    return append_bounded(
        output,
        std::string_view{buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())});
}

[[nodiscard]] bool append_field_value(std::string& output, const LogValue& value)
{
    return std::visit(
        [&output](const auto item) {
            using Value = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Value, bool>)
            {
                return append_bounded(output, item ? "true" : "false");
            }
            else
            {
                return append_integer(output, item);
            }
        },
        value);
}

[[nodiscard]] bool valid_fields(const std::initializer_list<LogField> fields) noexcept
{
    if (fields.size() > detail::max_log_fields)
    {
        return false;
    }

    for (auto current = fields.begin(); current != fields.end(); ++current)
    {
        if (field_name(current->key).empty())
        {
            return false;
        }
        for (auto earlier = fields.begin(); earlier != current; ++earlier)
        {
            if (earlier->key == current->key)
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::string format_thread_id()
{
    std::ostringstream formatted;
    formatted.imbue(std::locale::classic());
    formatted << std::this_thread::get_id();
    if (!formatted)
    {
        throw std::runtime_error{"Failed to format the current thread ID"};
    }
    return formatted.str();
}

} // namespace

namespace detail {
namespace {

void append_two_digits(std::string& output, const unsigned int value)
{
    output.push_back(static_cast<char>('0' + (value / 10U)));
    output.push_back(static_cast<char>('0' + (value % 10U)));
}

void append_three_digits(std::string& output, const unsigned int value)
{
    output.push_back(static_cast<char>('0' + (value / 100U)));
    append_two_digits(output, value % 100U);
}

[[nodiscard]] bool append_fixed_year(std::string& output, const int year)
{
    if (year < 0 || year > 9'999)
    {
        return false;
    }
    output.push_back(static_cast<char>('0' + ((year / 1'000) % 10)));
    output.push_back(static_cast<char>('0' + ((year / 100) % 10)));
    output.push_back(static_cast<char>('0' + ((year / 10) % 10)));
    output.push_back(static_cast<char>('0' + (year % 10)));
    return true;
}

} // namespace

std::optional<std::string> format_utc_timestamp(
    const std::chrono::system_clock::time_point timestamp)
{
    using namespace std::chrono;

    const auto millisecond_point = floor<milliseconds>(timestamp);
    const auto day_point = floor<days>(millisecond_point);
    const year_month_day date{day_point};
    const int year = static_cast<int>(date.year());
    if (!date.ok() || year < 0 || year > 9'999)
    {
        return std::nullopt;
    }

    const auto milliseconds_since_midnight =
        duration_cast<milliseconds>(millisecond_point - day_point).count();
    const auto hours = static_cast<unsigned int>(milliseconds_since_midnight / 3'600'000);
    const auto minutes = static_cast<unsigned int>((milliseconds_since_midnight / 60'000) % 60);
    const auto seconds = static_cast<unsigned int>((milliseconds_since_midnight / 1'000) % 60);
    const auto millis = static_cast<unsigned int>(milliseconds_since_midnight % 1'000);

    std::string result;
    result.reserve(24U);
    if (!append_fixed_year(result, year))
    {
        return std::nullopt;
    }
    result.push_back('-');
    append_two_digits(result, static_cast<unsigned int>(date.month()));
    result.push_back('-');
    append_two_digits(result, static_cast<unsigned int>(date.day()));
    result.push_back('T');
    append_two_digits(result, hours);
    result.push_back(':');
    append_two_digits(result, minutes);
    result.push_back(':');
    append_two_digits(result, seconds);
    result.push_back('.');
    append_three_digits(result, millis);
    result.push_back('Z');
    return result;
}

std::optional<std::string> format_record(
    const LogLevel level,
    const LogEvent event,
    const std::initializer_list<LogField> fields,
    const std::chrono::system_clock::time_point timestamp,
    const std::string_view thread_id)
{
    const std::string_view severity = level_name(level);
    const std::string_view event_text = event_name(event);
    if (severity.empty() || event_text.empty() || !valid_fields(fields) ||
        thread_id.size() > max_thread_id_size_bytes)
    {
        return std::nullopt;
    }

    const auto formatted_timestamp = format_utc_timestamp(timestamp);
    if (!formatted_timestamp.has_value())
    {
        return std::nullopt;
    }

    std::string record;
    record.reserve(max_log_record_size_bytes);
    if (!append_bounded(record, "{\"timestamp\":\""sv) ||
        !append_bounded(record, *formatted_timestamp) ||
        !append_bounded(record, "\",\"level\":\""sv) ||
        !append_bounded(record, severity) ||
        !append_bounded(record, "\",\"thread_id\":"sv) ||
        !append_json_string(record, thread_id) ||
        !append_bounded(record, ",\"event\":\""sv) ||
        !append_bounded(record, event_text) ||
        !append_bounded(record, "\",\"fields\":{"sv))
    {
        return std::nullopt;
    }

    bool first = true;
    for (const LogField& field : fields)
    {
        if (!first && !append_bounded(record, ","sv))
        {
            return std::nullopt;
        }
        first = false;
        if (!append_bounded(record, "\""sv) ||
            !append_bounded(record, field_name(field.key)) ||
            !append_bounded(record, "\":"sv) ||
            !append_field_value(record, field.value))
        {
            return std::nullopt;
        }
    }

    if (!append_bounded(record, "}}"sv))
    {
        return std::nullopt;
    }
    return record;
}

} // namespace detail

Logger::Logger(std::ostream& output, const LogLevel minimum)
    : output_{output},
      minimum_{minimum}
{
    if (severity_rank(minimum_) < 0)
    {
        throw std::invalid_argument{"Logger minimum severity is invalid"};
    }
}

void Logger::log(
    const LogLevel level,
    const LogEvent event,
    const std::initializer_list<LogField> fields) noexcept
{
    const int level_value = severity_rank(level);
    if (level_value < 0 || level_value < severity_rank(minimum_) ||
        event_name(event).empty() || !valid_fields(fields) ||
        output_failed_.load(std::memory_order_acquire))
    {
        return;
    }

    try
    {
        const auto record = detail::format_record(
            level, event, fields, std::chrono::system_clock::now(), format_thread_id());
        if (!record.has_value())
        {
            return;
        }

        std::lock_guard lock{output_mutex_};
        if (output_failed_.load(std::memory_order_acquire))
        {
            return;
        }

        try
        {
            output_.write(record->data(), static_cast<std::streamsize>(record->size()));
            if (!output_)
            {
                output_failed_.store(true, std::memory_order_release);
                return;
            }

            output_.put('\n');
            if (!output_)
            {
                output_failed_.store(true, std::memory_order_release);
                return;
            }

            output_.flush();
            if (!output_)
            {
                output_failed_.store(true, std::memory_order_release);
            }
        }
        catch (...)
        {
            output_failed_.store(true, std::memory_order_release);
        }
    }
    catch (...)
    {
        // Formatting or mutex failures drop only this record.
    }
}

bool Logger::output_failed() const noexcept
{
    return output_failed_.load(std::memory_order_acquire);
}

} // namespace telemetryhub::support
