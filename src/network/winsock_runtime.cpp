#include <telemetryhub/network/winsock_runtime.hpp>

#include <telemetryhub/network/winsock_error.hpp>

#include <winsock2.h>

#include <system_error>

namespace telemetryhub::network {

WinsockRuntime::WinsockRuntime()
{
    constexpr WORD requested_version = MAKEWORD(2, 2);
    WSADATA data{};
    const int startup_result = WSAStartup(requested_version, &data);
    if (startup_result != 0)
    {
        throw std::system_error{
            make_winsock_error_code(startup_result), "WSAStartup failed"};
    }

    if (data.wVersion != requested_version)
    {
        static_cast<void>(WSACleanup());
        throw std::system_error{
            make_winsock_error_code(WSAVERNOTSUPPORTED),
            "Winsock 2.2 is not available"};
    }
}

WinsockRuntime::~WinsockRuntime() noexcept
{
    static_cast<void>(WSACleanup());
}

} // namespace telemetryhub::network
