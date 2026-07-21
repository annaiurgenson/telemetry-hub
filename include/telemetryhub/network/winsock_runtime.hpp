#pragma once

namespace telemetryhub::network {

class WinsockRuntime
{
public:
    WinsockRuntime();
    ~WinsockRuntime() noexcept;

    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
    WinsockRuntime(WinsockRuntime&&) = delete;
    WinsockRuntime& operator=(WinsockRuntime&&) = delete;
};

} // namespace telemetryhub::network
