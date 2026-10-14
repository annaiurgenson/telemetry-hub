#include <telemetryhub/domain/telemetry_processor.hpp>
#include <telemetryhub/domain/telemetry_store.hpp>
#include <telemetryhub/network/tcp_server.hpp>
#include <telemetryhub/server/command_ingress.hpp>
#include <telemetryhub/server/device_registration_registry.hpp>
#include <telemetryhub/server/protocol_session_handler.hpp>

#include <iostream>
#include <memory>
#include <utility>

int main()
{
    telemetryhub::domain::TelemetryStore store;
    telemetryhub::domain::TelemetryProcessor processor{store};
    auto ingress = std::make_shared<telemetryhub::server::CommandIngress>(processor);
    auto registrations = std::make_shared<telemetryhub::server::DeviceRegistrationRegistry>();
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
        // The response sink captures server by reference. Drain and join ingress
        // before server teardown on every exceptional exit as well.
        ingress->shutdown();
        server.shutdown();
        throw;
    }

    // Stop producers, drain accepted commands through the live server, then stop
    // sessions. Processor and store outlive ingress and all session callbacks.
    ingress->shutdown();
    server.shutdown();
    return server.shutdown_error() ? 1 : 0;
}
