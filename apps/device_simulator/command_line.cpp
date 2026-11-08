#include "command_line.hpp"

#include <charconv>
#include <system_error>

namespace device_simulator {
namespace {

[[nodiscard]] ParseResult make_error(
    const std::string_view diagnostic,
    const std::string_view argument = {}) noexcept
{
    return {ParseStatus::error, 0U, diagnostic, argument};
}

} // namespace

ParseResult parse_command_line(const int argc, const char* const argv[]) noexcept
{
    if (argc == 2 && std::string_view{argv[1]} == "--help")
    {
        return {ParseStatus::help, 0U, {}, {}};
    }
    if (argc < 2)
    {
        return make_error("--port is required");
    }

    bool port_seen = false;
    std::uint16_t port = 0U;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view option{argv[index]};
        if (option == "--help")
        {
            return make_error("--help must be used by itself", option);
        }
        if (option != "--port")
        {
            return make_error("unknown argument", option);
        }
        if (port_seen)
        {
            return make_error("option specified more than once", option);
        }
        if (index + 1 >= argc)
        {
            return make_error("missing value for --port");
        }

        const std::string_view value{argv[++index]};
        unsigned int parsed = 0U;
        const char* const first = value.data();
        const char* const last = first + value.size();
        const auto [end, error] = std::from_chars(first, last, parsed, 10);
        if (value.empty() || error != std::errc{} || end != last)
        {
            return make_error("invalid decimal value for --port", value);
        }
        if (parsed == 0U || parsed > 65'535U)
        {
            return make_error("--port value must be in range 1..65535", value);
        }

        port = static_cast<std::uint16_t>(parsed);
        port_seen = true;
    }

    if (!port_seen)
    {
        return make_error("--port is required");
    }
    return {ParseStatus::success, port, {}, {}};
}

std::string_view usage_text() noexcept
{
    return "Usage: device_simulator --port <number>\n"
           "       device_simulator --help\n"
           "Connects to 127.0.0.1 and submits a fixed temperature demonstration.\n"
           "--port accepts 1..65535.\n";
}

} // namespace device_simulator
