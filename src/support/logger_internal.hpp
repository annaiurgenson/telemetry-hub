#pragma once

#include <telemetryhub/support/logger.hpp>

#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace telemetryhub::support::detail {

inline constexpr std::size_t max_log_fields = 4U;
inline constexpr std::size_t max_thread_id_size_bytes = 128U;
inline constexpr std::size_t max_log_record_size_bytes = 512U;

[[nodiscard]] std::optional<std::string> format_utc_timestamp(
    std::chrono::system_clock::time_point timestamp);

[[nodiscard]] std::optional<std::string> format_record(
    LogLevel level,
    LogEvent event,
    std::initializer_list<LogField> fields,
    std::chrono::system_clock::time_point timestamp,
    std::string_view thread_id);

} // namespace telemetryhub::support::detail
