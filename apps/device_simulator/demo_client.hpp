#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>

namespace device_simulator {

struct DemoOptions
{
    std::uint16_t port{};
    std::optional<std::uint32_t> interval_ms;
    std::optional<std::uint32_t> count;
};

void run_demo(std::uint16_t port, std::ostream& output);
void run_demo(const DemoOptions& options, std::ostream& output);

} // namespace device_simulator
