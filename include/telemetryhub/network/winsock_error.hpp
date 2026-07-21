#pragma once

#include <system_error>

namespace telemetryhub::network {

[[nodiscard]] const std::error_category& winsock_error_category() noexcept;
[[nodiscard]] std::error_code make_winsock_error_code(int error) noexcept;

} // namespace telemetryhub::network
