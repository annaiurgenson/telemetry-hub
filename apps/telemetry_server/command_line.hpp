#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace telemetry_server {

struct ConfigurationOverrides
{
    std::optional<std::uint16_t> port;
    std::optional<std::size_t> max_connections;
};

enum class ParseStatus
{
    success,
    help,
    error
};

struct ParseResult
{
    ParseStatus status{ParseStatus::error};
    ConfigurationOverrides overrides;
    std::string_view diagnostic;
    std::string_view argument;
};

[[nodiscard]] ParseResult parse_command_line(int argc, const char* const argv[]) noexcept;
[[nodiscard]] std::string_view usage_text() noexcept;

} // namespace telemetry_server
