#include "demo_client.hpp"

#include <telemetryhub/concurrency/worker_pool.hpp>
#include <telemetryhub/domain/telemetry_processor.hpp>
#include <telemetryhub/domain/telemetry_store.hpp>
#include <telemetryhub/network/tcp_server.hpp>
#include <telemetryhub/server/command_ingress.hpp>
#include <telemetryhub/server/device_registration_registry.hpp>
#include <telemetryhub/server/protocol_session_handler.hpp>
#include <telemetryhub/support/logger.hpp>

#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

class DemoServer
{
public:
    DemoServer()
        : logger_{logs_},
          ingress_{std::make_shared<telemetryhub::server::CommandIngress>(
              processor_, worker_pool_,
              telemetryhub::server::default_incoming_command_queue_capacity, &logger_)},
          handler_{std::make_shared<telemetryhub::server::ProtocolSessionHandler>(
              registrations_, ingress_, &logger_)},
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

    [[nodiscard]] bool wait_for_disconnect()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (server_.active_connection_count() != 0U &&
               std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::yield();
        }
        return server_.active_connection_count() == 0U;
    }

    [[nodiscard]] std::string logs_after_disconnect() const
    {
        return logs_.str();
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

    std::ostringstream logs_;
    telemetryhub::support::Logger logger_;
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

[[nodiscard]] std::optional<double> mean_for_sample_count(
    const std::string_view server_output,
    const std::size_t sample_count)
{
    const std::string count_marker =
        "\"sample_count\":" + std::to_string(sample_count);
    const std::size_t count_position = server_output.find(count_marker);
    if (count_position == std::string_view::npos)
    {
        return std::nullopt;
    }

    constexpr std::string_view mean_marker{"\"mean\":"};
    const std::size_t mean_position = server_output.find(mean_marker, count_position);
    if (mean_position == std::string_view::npos)
    {
        return std::nullopt;
    }

    const char* const mean_begin = server_output.data() + mean_position + mean_marker.size();
    double mean{};
    const auto parsed = std::from_chars(
        mean_begin, server_output.data() + server_output.size(), mean);
    if (parsed.ec != std::errc{})
    {
        return std::nullopt;
    }
    return mean;
}

TEST(DeviceSimulator, RegistersSubmitsSamplesAndPrintsEachConfirmedValue)
{
    DemoServer server;
    std::ostringstream output;

    device_simulator::run_demo(server.port(), output);

    const std::string transcript = output.str();
    EXPECT_NE(transcript.find("Connected to 127.0.0.1:"), std::string::npos);
    EXPECT_NE(transcript.find("Registered device demo-sensor-1"), std::string::npos);
    EXPECT_NE(transcript.find("Telemetry accepted: temperature=21.50"), std::string::npos);
    EXPECT_NE(transcript.find("Telemetry accepted: temperature=22.00"), std::string::npos);
    EXPECT_NE(transcript.find("Telemetry accepted: temperature=23.50"), std::string::npos);
    std::size_t acknowledgements = 0U;
    std::size_t position = 0U;
    while ((position = transcript.find("Telemetry accepted:", position)) != std::string::npos)
    {
        ++acknowledgements;
        ++position;
    }
    EXPECT_EQ(acknowledgements, 3U);
    ASSERT_TRUE(server.wait_for_disconnect())
        << "Timed out waiting for the simulator connection and disconnect log to finish";
    const std::string server_output = server.logs_after_disconnect();
    EXPECT_NE(server_output.find("\"event\":\"device_registered\""), std::string::npos);
    EXPECT_NE(server_output.find("\"active_device_count\":1"), std::string::npos);
    EXPECT_NE(server_output.find("\"event\":\"device_disconnected\""), std::string::npos);
    EXPECT_NE(server_output.find("\"active_device_count\":0"), std::string::npos);
    EXPECT_NE(server_output.find("\"event\":\"telemetry_accepted\""), std::string::npos);
    EXPECT_NE(
        server_output.find(
            "\"value\":21.5,\"sample_count\":1,\"minimum\":21.5,\"maximum\":21.5,\"mean\":21.5"),
        std::string::npos);
    EXPECT_NE(
        server_output.find(
            "\"value\":22,\"sample_count\":2,\"minimum\":21.5,\"maximum\":22,\"mean\":21.75"),
        std::string::npos);
    const auto mean = mean_for_sample_count(server_output, 3U);
    ASSERT_TRUE(mean.has_value());
    EXPECT_NEAR(*mean, 67.0 / 3.0, 1e-12);
}

TEST(DeviceSimulator, RepeatedRunInOneServerContinuesAccumulatingStatistics)
{
    DemoServer server;
    std::ostringstream first_output;
    std::ostringstream second_output;

    device_simulator::run_demo(server.port(), first_output);
    ASSERT_TRUE(server.wait_for_disconnect())
        << "Timed out waiting for the first simulator connection to finish; not reconnecting";
    device_simulator::run_demo(server.port(), second_output);
    ASSERT_TRUE(server.wait_for_disconnect())
        << "Timed out waiting for the second simulator connection and logs to finish";

    EXPECT_EQ(first_output.str().find("Statistics for"), std::string::npos);
    EXPECT_NE(first_output.str().find("Telemetry accepted: temperature=23.50"), std::string::npos);
    const std::string logs = server.logs_after_disconnect();
    EXPECT_NE(logs.find("\"sample_count\":6"), std::string::npos);
    EXPECT_NE(second_output.str().find("Telemetry accepted: temperature=23.50"), std::string::npos);
    const auto mean = mean_for_sample_count(logs, 6U);
    ASSERT_TRUE(mean.has_value());
    EXPECT_NEAR(*mean, 67.0 / 3.0, 1e-12);
}

} // namespace
