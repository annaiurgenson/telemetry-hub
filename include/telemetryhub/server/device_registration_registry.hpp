#pragma once

#include <telemetryhub/network/tcp_server.hpp>

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace telemetryhub::server {

enum class RegistrationResult
{
    registered,
    already_registered,
    connection_already_registered,
    device_in_use
};

// One mutex linearizes registration and ConnectionId-checked disconnect cleanup.
class DeviceRegistrationRegistry
{
public:
    [[nodiscard]] RegistrationResult register_device(
        network::ConnectionId connection_id,
        std::string_view device_id);
    void erase_connection(network::ConnectionId connection_id) noexcept;
    [[nodiscard]] bool is_registered(network::ConnectionId connection_id) const noexcept;
    [[nodiscard]] std::optional<std::string> device_for_connection(
        network::ConnectionId connection_id) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    mutable std::mutex mutex_;
    std::map<std::string, network::ConnectionId, std::less<>> owners_;
};

} // namespace telemetryhub::server