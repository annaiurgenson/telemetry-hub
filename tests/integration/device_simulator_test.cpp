#include "demo_client.hpp"

#include <telemetryhub/concurrency/worker_pool.hpp>
#include <telemetryhub/domain/telemetry_processor.hpp>
#include <telemetryhub/domain/telemetry_store.hpp>
#include <telemetryhub/network/tcp_server.hpp>
#include <telemetryhub/server/command_ingress.hpp>
#include <telemetryhub/server/device_registration_registry.hpp>
#include <telemetryhub/server/protocol_session_handler.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class DemoServer
{
public:
    DemoServer()
        : ingress_{std::make_shared<telemetryhub::server::CommandIngress>(
              processor_, worker_pool_)},
          handler_{std::make_shared<telemetryhub::server::ProtocolSessionHandler>(
              registrations_, ingress_)},
          server_{make_config(handler_)}
    {
        ingress_->start([this](const telemetryhub::network::ConnectionId id,
                               std::vector<std::byte> bytes) {
            return server_.try_enqueue(id, std::move(bytes));
        });
        server_.start();
    }

    ~DemoServer()
    {
        ingress_->shutdown();
        worker_pool_.shutdown();
        server_.shutdown();
    }

    [[nodiscard]] std::uint16_t port() const
    {
        return server_.bound_port();
    }

private:
    [[nodiscard]] static telemetryhub::network::TcpServerConfig make_config(
        const std::shared_ptr<telemetryhub::server::ProtocolSessionHandler>& handler)
    {
        telemetryhub::network::TcpServerConfig config;
        config.session_callbacks_factory = [handler](const telemetryhub::network::ConnectionId id) {
            return handler->callbacks_for(id);
        };
        return config;
    }

    telemetryhub::domain::TelemetryStore store_;
    telemetryhub::domain::TelemetryProcessor processor_{store_};
    telemetryhub::concurrency::WorkerPool worker_pool_{
        telemetryhub::server::default_telemetry_worker_count,
        telemetryhub::server::default_worker_pool_task_queue_capacity};
    std::shared_ptr<telemetryhub::server::DeviceRegistrationRegistry> registrations_{
        std::make_shared<telemetryhub::server::DeviceRegistrationRegistry>()};
    std::shared_ptr<telemetryhub::server::CommandIngress> ingress_;
    std::shared_ptr<telemetryhub::server::ProtocolSessionHandler> handler_;
    telemetryhub::network::TcpServer server_;
};

TEST(DeviceSimulator, RegistersSubmitsSamplesAndPrintsCorrelatedStatistics)
{
    DemoServer server;
    std::ostringstream output;

    device_simulator::run_demo(server.port(), output);

    const std::string transcript = output.str();
    EXPECT_NE(transcript.find("Connected to 127.0.0.1:"), std::string::npos);
    EXPECT_NE(transcript.find("Registered device demo-sensor-1"), std::string::npos);
    std::size_t acknowledgements = 0U;
    std::size_t position = 0U;
    while ((position = transcript.find("Telemetry accepted:", position)) != std::string::npos)
    {
        ++acknowledgements;
        ++position;
    }
    EXPECT_EQ(acknowledgements, 3U);
    EXPECT_NE(transcript.find("Statistics for temperature:"), std::string::npos);
    EXPECT_NE(transcript.find("sample_count: 3"), std::string::npos);
    EXPECT_NE(transcript.find("minimum: 21.50"), std::string::npos);
    EXPECT_NE(transcript.find("maximum: 23.50"), std::string::npos);
    EXPECT_NE(transcript.find("mean: 22.33"), std::string::npos);
    EXPECT_NE(transcript.find("last_value: 23.50"), std::string::npos);
    constexpr std::string_view timestamp_label{"last_received_at: "};
    const std::size_t timestamp_start = transcript.find(timestamp_label);
    ASSERT_NE(timestamp_start, std::string::npos);
    const std::size_t timestamp_value_start = timestamp_start + timestamp_label.size();
    const std::size_t timestamp_end = transcript.find('\n', timestamp_value_start);
    ASSERT_NE(timestamp_end, std::string::npos);
    const std::string_view timestamp{
        transcript.data() + timestamp_value_start, timestamp_end - timestamp_value_start};
    ASSERT_EQ(timestamp.size(), 24U);
    EXPECT_EQ(timestamp.back(), 'Z');
}

} // namespace
