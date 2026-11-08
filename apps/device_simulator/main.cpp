#include "command_line.hpp"
#include "demo_client.hpp"

#include <exception>
#include <iostream>

int main(const int argc, char* argv[])
{
    const device_simulator::ParseResult parsed =
        device_simulator::parse_command_line(argc, argv);
    if (parsed.status == device_simulator::ParseStatus::help)
    {
        std::cout << device_simulator::usage_text();
        return 0;
    }
    if (parsed.status == device_simulator::ParseStatus::error)
    {
        std::cerr << "device_simulator: " << parsed.diagnostic;
        if (!parsed.argument.empty())
        {
            std::cerr << ": " << parsed.argument;
        }
        std::cerr << '\n' << device_simulator::usage_text();
        return 2;
    }

    try
    {
        device_simulator::run_demo(parsed.port, std::cout);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "device_simulator: " << error.what() << '\n';
        return 1;
    }
    catch (...)
    {
        std::cerr << "device_simulator: unexpected failure\n";
        return 1;
    }
}
