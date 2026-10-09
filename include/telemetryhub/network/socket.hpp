#pragma once

#include <winsock2.h>

#include <system_error>

namespace telemetryhub::network {

class Socket
{
public:
    using native_handle_type = SOCKET;
    static constexpr native_handle_type invalid_handle = INVALID_SOCKET;

    Socket() noexcept = default;
    explicit Socket(native_handle_type handle) noexcept;
    ~Socket() noexcept;

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] native_handle_type native_handle() const noexcept;
    [[nodiscard]] native_handle_type release() noexcept;

    // Configure before starting any threads that borrow the handle.
    [[nodiscard]] std::error_code set_nonblocking(bool enabled) noexcept;

    // A replacement is adopted only if closing the current handle succeeds.
    // On failure, this Socket retains its current handle; the caller retains
    // ownership of replacement, which is neither closed nor adopted.
    [[nodiscard]] std::error_code reset(native_handle_type replacement = invalid_handle) noexcept;
    [[nodiscard]] std::error_code close() noexcept;

private:
    native_handle_type handle_{invalid_handle};
};

} // namespace telemetryhub::network
