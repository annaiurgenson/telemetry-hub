#pragma once

#include <telemetryhub/network/tcp_server.hpp>
#include <telemetryhub/server/device_registration_registry.hpp>

#include <memory>

namespace telemetryhub::server {

class ProtocolSessionHandler
{
public:
    explicit ProtocolSessionHandler(std::shared_ptr<DeviceRegistrationRegistry> registrations);

    [[nodiscard]] network::SessionCallbacks callbacks_for(network::ConnectionId connection_id) const;
    [[nodiscard]] std::shared_ptr<DeviceRegistrationRegistry> registrations() const noexcept;

private:
    std::shared_ptr<DeviceRegistrationRegistry> registrations_;
};

} // namespace telemetryhub::server