#pragma once

#include <cstdint>
#include <string_view>

namespace device_simulator {

enum class ParseStatus
{
    success,
    help,
    error
};

struct ParseResult
{
    ParseStatus status{ParseStatus::error};
    std::uint16_t port{};
    std::string_view diagnostic;
    std::string_view argument;
};

[[nodiscard]] ParseResult parse_command_line(int argc, const char* const argv[]) noexcept;
[[nodiscard]] std::string_view usage_text() noexcept;

} // namespace device_simulator
