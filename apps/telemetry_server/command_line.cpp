#include "command_line.hpp"

#include <charconv>
#include <limits>
#include <system_error>

namespace telemetry_server {
namespace {

[[nodiscard]] bool parse_decimal(const std::string_view text, std::uint64_t& value) noexcept
{
    if (text.empty())
    {
        return false;
    }

    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [end, error] = std::from_chars(first, last, value, 10);
    return error == std::errc{} && end == last;
}

[[nodiscard]] ParseResult make_error(
    const std::string_view diagnostic,
    const std::string_view argument = {}) noexcept
{
    return {ParseStatus::error, {}, diagnostic, argument};
}

} // namespace

ParseResult parse_command_line(const int argc, const char* const argv[]) noexcept
{
    if (argc <= 1)
    {
        return {ParseStatus::success, {}, {}, {}};
    }

    ConfigurationOverrides overrides;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view option{argv[index]};
        if (option == "--help")
        {
            if (argc == 2)
            {
                return {ParseStatus::help, {}, {}, {}};
            }
            return make_error("--help must be used by itself", option);
        }

        if (option == "--port")
        {
            if (overrides.port.has_value())
            {
                return make_error("option specified more than once", option);
            }
            if (index + 1 >= argc)
            {
                return make_error("missing value for --port");
            }

            const std::string_view value{argv[++index]};
            std::uint64_t parsed = 0U;
            if (!parse_decimal(value, parsed))
            {
                return make_error("invalid decimal value for --port", value);
            }
            if (parsed > 65'535U)
            {
                return make_error("--port value must be in range 0..65535", value);
            }
            overrides.port = static_cast<std::uint16_t>(parsed);
            continue;
        }

        if (option == "--max-connections")
        {
            if (overrides.max_connections.has_value())
            {
                return make_error("option specified more than once", option);
            }
            if (index + 1 >= argc)
            {
                return make_error("missing value for --max-connections");
            }

            const std::string_view value{argv[++index]};
            std::uint64_t parsed = 0U;
            if (!parse_decimal(value, parsed))
            {
                return make_error("invalid decimal value for --max-connections", value);
            }
            constexpr auto maximum_connections =
                static_cast<std::uint64_t>((std::numeric_limits<int>::max)());
            if (parsed == 0U || parsed > maximum_connections)
            {
                return make_error(
                    "--max-connections value must be in range 1..INT_MAX", value);
            }
            overrides.max_connections = static_cast<std::size_t>(parsed);
            continue;
        }

        return make_error("unknown argument", option);
    }

    return {ParseStatus::success, overrides, {}, {}};
}

std::string_view usage_text() noexcept
{
    return "Usage: telemetry_server [--port <number>] [--max-connections <number>]\n"
           "       telemetry_server --help\n"
           "--port accepts 0..65535; 0 requests an OS-selected ephemeral port.\n"
           "--max-connections accepts 1..INT_MAX.\n";
}

} // namespace telemetry_server
