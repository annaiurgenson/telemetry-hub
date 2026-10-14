#include <telemetryhub/network/tcp_server.hpp>
#include <telemetryhub/server/command_ingress.hpp>
#include <telemetryhub/server/device_registration_registry.hpp>
#include <telemetryhub/server/protocol_session_handler.hpp>

#include <iostream>
#include <memory>
#include <utility>

int main()
{
    auto registrations = std::make_shared<telemetryhub::server::DeviceRegistrationRegistry>();
    auto ingress = std::make_shared<telemetryhub::server::CommandIngress>();
    auto handler = std::make_shared<telemetryhub::server::ProtocolSessionHandler>(registrations, ingress);

    telemetryhub::network::TcpServerConfig config;
    config.session_callbacks_factory = [handler](const telemetryhub::network::ConnectionId id) {
        return handler->callbacks_for(id);
    };

    telemetryhub::network::TcpServer server{std::move(config)};
    ingress->start([&server](const telemetryhub::network::ConnectionId id,
                             std::vector<std::byte> bytes) {
        return server.try_enqueue(id, std::move(bytes));
    });
    try
    {
        server.start();
        std::cout << "TelemetryHub listening on loopback port " << server.bound_port()
                  << ". Press Enter to stop.\n";
        std::cin.get();
    }
    catch (...)
    {
        // The response sink captures server by reference, so drain and join the
        // consumer before server teardown on every exceptional exit as well.
        ingress->shutdown();
        server.shutdown();
        throw;
    }

    // Stop producers first, drain accepted commands through the live transport,
    // then stop sessions. Callback owners remain alive until server shutdown joins them.
    ingress->shutdown();
    server.shutdown();
    return server.shutdown_error() ? 1 : 0;
}
