#include "command_line.hpp"

#include <charconv>
#include <optional>
#include <system_error>

namespace device_simulator {
namespace {

[[nodiscard]] ParseResult make_error(
    const std::string_view diagnostic,
    const std::string_view argument = {}) noexcept
{
    return {ParseStatus::error, 0U, {}, {}, diagnostic, argument};
}

[[nodiscard]] std::optional<std::uint32_t> parse_decimal(
    const std::string_view value) noexcept
{
    if (value.empty())
    {
        return std::nullopt;
    }

    std::uint32_t parsed = 0U;
    const char* const first = value.data();
    const char* const last = first + value.size();
    const auto [end, error] = std::from_chars(first, last, parsed, 10);
    if (error != std::errc{} || end != last)
    {
        return std::nullopt;
    }
    return parsed;
}

} // namespace

ParseResult parse_command_line(const int argc, const char* const argv[]) noexcept
{
    if (argc == 2 && std::string_view{argv[1]} == "--help")
    {
        return {ParseStatus::help, 0U, {}, {}, {}, {}};
    }
    if (argc < 2)
    {
        return make_error("--port is required");
    }

    bool port_seen = false;
    bool interval_seen = false;
    bool count_seen = false;
    std::uint16_t port = 0U;
    std::optional<std::uint32_t> interval_ms;
    std::optional<std::uint32_t> count;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view option{argv[index]};
        if (option == "--help")
        {
            return make_error("--help must be used by itself", option);
        }
        bool* seen = nullptr;
        if (option == "--port")
        {
            seen = &port_seen;
        }
        else if (option == "--interval-ms")
        {
            seen = &interval_seen;
        }
        else if (option == "--count")
        {
            seen = &count_seen;
        }
        else
        {
            return make_error("unknown argument", option);
        }
        if (*seen)
        {
            return make_error("option specified more than once", option);
        }
        *seen = true;
        if (index + 1 >= argc)
        {
            return make_error("missing value for option", option);
        }

        const std::string_view value{argv[++index]};
        const auto parsed = parse_decimal(value);
        if (!parsed.has_value())
        {
            return make_error("invalid decimal value for option", value);
        }
        if (option == "--port")
        {
            if (*parsed == 0U || *parsed > 65'535U)
            {
                return make_error("--port value must be in range 1..65535", value);
            }
            port = static_cast<std::uint16_t>(*parsed);
        }
        else if (option == "--interval-ms")
        {
            if (*parsed == 0U || *parsed > 60'000U)
            {
                return make_error("--interval-ms value must be in range 1..60000", value);
            }
            interval_ms = *parsed;
        }
        else
        {
            if (*parsed == 0U || *parsed > 1'000U)
            {
                return make_error("--count value must be in range 1..1000", value);
            }
            count = *parsed;
        }
    }

    if (!port_seen)
    {
        return make_error("--port is required");
    }
    if (interval_ms.has_value() != count.has_value())
    {
        return make_error("--interval-ms and --count must be supplied together");
    }
    return {ParseStatus::success, port, interval_ms, count, {}, {}};
}

std::string_view usage_text() noexcept
{
    return "Usage: device_simulator --port <number>\n"
           "       device_simulator --port <number> --interval-ms <1..60000> --count <1..1000>\n"
           "       device_simulator --help\n"
           "Connects to 127.0.0.1 and submits a fixed temperature demonstration.\n"
           "--port accepts 1..65535.\n"
           "Periodic mode requires both --interval-ms and --count.\n";
}

} // namespace device_simulator
