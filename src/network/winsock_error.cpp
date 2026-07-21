#include <telemetryhub/network/winsock_error.hpp>

#include <winsock2.h>

#include <string>

namespace telemetryhub::network {
namespace {

class WinsockErrorCategory final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override
    {
        return "winsock";
    }

    [[nodiscard]] std::string message(const int condition) const override
    {
        switch (condition)
        {
        case WSAEINTR:
            return "Interrupted operation";
        case WSAEACCES:
            return "Permission denied";
        case WSAEFAULT:
            return "Invalid memory address";
        case WSAEINVAL:
            return "Invalid argument";
        case WSAEMFILE:
            return "Too many open sockets";
        case WSAEWOULDBLOCK:
            return "Operation would block";
        case WSAEINPROGRESS:
            return "Operation in progress";
        case WSAEALREADY:
            return "Operation already in progress";
        case WSAENOTSOCK:
            return "Not a socket";
        case WSAEMSGSIZE:
            return "Message too long";
        case WSAEPROTONOSUPPORT:
            return "Protocol not supported";
        case WSAEOPNOTSUPP:
            return "Operation not supported";
        case WSAEAFNOSUPPORT:
            return "Address family not supported";
        case WSAEADDRINUSE:
            return "Address already in use";
        case WSAEADDRNOTAVAIL:
            return "Address not available";
        case WSAENETDOWN:
            return "Network is down";
        case WSAENETUNREACH:
            return "Network is unreachable";
        case WSAENETRESET:
            return "Network connection reset";
        case WSAECONNABORTED:
            return "Connection aborted";
        case WSAECONNRESET:
            return "Connection reset";
        case WSAENOBUFS:
            return "No buffer space available";
        case WSAEISCONN:
            return "Socket is already connected";
        case WSAENOTCONN:
            return "Socket is not connected";
        case WSAETIMEDOUT:
            return "Operation timed out";
        case WSAECONNREFUSED:
            return "Connection refused";
        case WSAEHOSTUNREACH:
            return "Host is unreachable";
        case WSASYSNOTREADY:
            return "Network subsystem is unavailable";
        case WSAVERNOTSUPPORTED:
            return "Requested Winsock version is not supported";
        case WSANOTINITIALISED:
            return "Winsock is not initialized";
        default:
            return "Unknown Winsock error " + std::to_string(condition);
        }
    }
};

} // namespace

const std::error_category& winsock_error_category() noexcept
{
    static const WinsockErrorCategory category;
    return category;
}

std::error_code make_winsock_error_code(const int error) noexcept
{
    return {error, winsock_error_category()};
}

} // namespace telemetryhub::network
