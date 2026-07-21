#include <telemetryhub/network/socket.hpp>

#include <telemetryhub/network/winsock_error.hpp>

#include <system_error>
#include <utility>

namespace telemetryhub::network {

Socket::Socket(const native_handle_type handle) noexcept
    : handle_{handle}
{
}

Socket::~Socket() noexcept
{
    static_cast<void>(close());
}

Socket::Socket(Socket&& other) noexcept
    : handle_{other.release()}
{
}

Socket& Socket::operator=(Socket&& other)
{
    if (this == &other)
    {
        return *this;
    }

    const std::error_code close_error = close();
    if (close_error)
    {
        throw std::system_error{close_error, "Failed to close socket during move assignment"};
    }

    handle_ = other.release();
    return *this;
}

bool Socket::valid() const noexcept
{
    return handle_ != invalid_handle;
}

Socket::native_handle_type Socket::native_handle() const noexcept
{
    return handle_;
}

Socket::native_handle_type Socket::release() noexcept
{
    return std::exchange(handle_, invalid_handle);
}

std::error_code Socket::reset(const native_handle_type replacement) noexcept
{
    if (handle_ == replacement)
    {
        return {};
    }

    const std::error_code close_error = close();
    if (close_error)
    {
        return close_error;
    }

    handle_ = replacement;
    return {};
}

std::error_code Socket::close() noexcept
{
    if (!valid())
    {
        return {};
    }

    if (closesocket(handle_) == SOCKET_ERROR)
    {
        return make_winsock_error_code(WSAGetLastError());
    }

    handle_ = invalid_handle;
    return {};
}

} // namespace telemetryhub::network
