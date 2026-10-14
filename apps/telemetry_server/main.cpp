#include <telemetryhub/concurrency/worker_pool.hpp>
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
    telemetryhub::concurrency::WorkerPool worker_pool{
        telemetryhub::server::default_telemetry_worker_count,
        telemetryhub::server::default_worker_pool_task_queue_capacity};
    auto ingress = std::make_shared<telemetryhub::server::CommandIngress>(processor, worker_pool);
    auto registrations = std::make_shared<telemetryhub::server::DeviceRegistrationRegistry>();
    auto handler = std::make_shared<telemetryhub::server::ProtocolSessionHandler>(registrations, ingress);

    telemetryhub::network::TcpServerConfig config;
    config.session_callbacks_factory = [handler](const telemetryhub::network::ConnectionId id) {
        return handler->callbacks_for(id);
    };

    telemetryhub::network::TcpServer server{std::move(config)};
    try
    {
        ingress->start([&server](const telemetryhub::network::ConnectionId id,
                                 std::vector<std::byte> bytes) {
            return server.try_enqueue(id, std::move(bytes));
        });
        server.start();
        std::cout << "TelemetryHub listening on loopback port " << server.bound_port()
                  << ". Press Enter to stop.\n";
        std::cin.get();
    }
    catch (...)
    {
        // The response sink captures server by reference. Drain dispatch and join
        // workers before server teardown on every exceptional exit as well.
        ingress->shutdown();
        worker_pool.shutdown();
        server.shutdown();
        throw;
    }

    // Stop producers, join dispatch, drain accepted worker tasks through the
    // live server, then stop sessions. Processor and store outlive all workers.
    ingress->shutdown();
    worker_pool.shutdown();
    server.shutdown();
    return server.shutdown_error() ? 1 : 0;
}
