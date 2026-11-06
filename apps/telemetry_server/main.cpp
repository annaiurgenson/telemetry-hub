#include <telemetryhub/concurrency/worker_pool.hpp>
#include <telemetryhub/domain/telemetry_processor.hpp>
#include <telemetryhub/domain/telemetry_store.hpp>
#include <telemetryhub/network/tcp_server.hpp>
#include <telemetryhub/server/command_ingress.hpp>
#include <telemetryhub/server/device_registration_registry.hpp>
#include <telemetryhub/server/protocol_session_handler.hpp>
#include <telemetryhub/support/logger.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <system_error>
#include <utility>

namespace {

using telemetryhub::support::LogEvent;
using telemetryhub::support::LogFieldKey;
using telemetryhub::support::LogLevel;
using telemetryhub::support::Logger;

int run_server(Logger& logger)
{
    telemetryhub::domain::TelemetryStore store;
    telemetryhub::domain::TelemetryProcessor processor{store};
    std::unique_ptr<telemetryhub::concurrency::WorkerPool> worker_pool;
    std::shared_ptr<telemetryhub::server::CommandIngress> ingress;
    std::shared_ptr<telemetryhub::server::DeviceRegistrationRegistry> registrations;
    std::shared_ptr<telemetryhub::server::ProtocolSessionHandler> handler;
    std::unique_ptr<telemetryhub::network::TcpServer> server;
    bool server_started = false;
    bool stopping_logged = false;

    const auto cleanup = [&]() noexcept {
        if (server_started && !stopping_logged)
        {
            logger.log(LogLevel::info, LogEvent::application_stopping);
            stopping_logged = true;
        }
        if (ingress)
        {
            ingress->shutdown();
        }
        if (worker_pool)
        {
            worker_pool->shutdown();
        }
        if (server)
        {
            server->shutdown();
        }
        return server ? server->shutdown_error() : std::error_code{};
    };

    try
    {
        worker_pool = std::make_unique<telemetryhub::concurrency::WorkerPool>(
            telemetryhub::server::default_telemetry_worker_count,
            telemetryhub::server::default_worker_pool_task_queue_capacity);
        ingress = std::make_shared<telemetryhub::server::CommandIngress>(
            processor, *worker_pool,
            telemetryhub::server::default_incoming_command_queue_capacity, &logger);
        registrations = std::make_shared<telemetryhub::server::DeviceRegistrationRegistry>();
        handler = std::make_shared<telemetryhub::server::ProtocolSessionHandler>(registrations, ingress);

        telemetryhub::network::TcpServerConfig config;
        config.logger = &logger;
        config.session_callbacks_factory = [handler](const telemetryhub::network::ConnectionId id) {
            return handler->callbacks_for(id);
        };

        server = std::make_unique<telemetryhub::network::TcpServer>(std::move(config));
        ingress->start([&server](const telemetryhub::network::ConnectionId id,
                                 std::vector<std::byte> bytes) {
            return server->try_enqueue(id, std::move(bytes));
        });
        server->start();
        server_started = true;
        const auto port = static_cast<std::uint64_t>(server->bound_port());
        logger.log(LogLevel::info, LogEvent::application_started,
                   {{LogFieldKey::port, port}});
        std::cout << "Press Enter to stop.\n";
        std::cin.get();
    }
    catch (...)
    {
        const std::error_code shutdown_error = cleanup();
        if (!server_started)
        {
            logger.log(LogLevel::error, LogEvent::application_start_failed);
        }
        else
        {
            logger.log(LogLevel::error, LogEvent::unknown_failure);
            if (shutdown_error)
            {
                const auto error_code = static_cast<std::int64_t>(shutdown_error.value());
                logger.log(LogLevel::error, LogEvent::application_stopped,
                           {{LogFieldKey::error_code, error_code}});
            }
            else
            {
                logger.log(LogLevel::info, LogEvent::application_stopped);
            }
        }
        return 1;
    }

    // Stop producers, join dispatch, drain accepted worker tasks through the
    // live server, then stop sessions. Processor and store outlive all workers.
    const std::error_code shutdown_error = cleanup();
    if (shutdown_error)
    {
        const auto error_code = static_cast<std::int64_t>(shutdown_error.value());
        logger.log(LogLevel::error, LogEvent::application_stopped,
                   {{LogFieldKey::error_code, error_code}});
        return 1;
    }
    logger.log(LogLevel::info, LogEvent::application_stopped);
    return 0;
}

} // namespace

int main()
{
    Logger logger{std::clog};
    return run_server(logger);
}
