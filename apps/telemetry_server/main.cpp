#include <telemetryhub/network/tcp_server.hpp>
#include <telemetryhub/server/device_registration_registry.hpp>
#include <telemetryhub/server/protocol_session_handler.hpp>

#include <iostream>
#include <memory>
#include <utility>

int main()
{
    auto registrations = std::make_shared<telemetryhub::server::DeviceRegistrationRegistry>();
    auto handler = std::make_shared<telemetryhub::server::ProtocolSessionHandler>(registrations);

    telemetryhub::network::TcpServerConfig config;
    config.session_callbacks_factory = [handler](const telemetryhub::network::ConnectionId id) {
        return handler->callbacks_for(id);
    };

    telemetryhub::network::TcpServer server{std::move(config)};
    server.start();
    std::cout << "TelemetryHub listening on loopback port " << server.bound_port()
              << ". Press Enter to stop.\n";
    std::cin.get();
    server.shutdown();
    return server.shutdown_error() ? 1 : 0;
}
