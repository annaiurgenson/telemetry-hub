#include "demo_client.hpp"

#include <telemetryhub/network/tcp.hpp>
#include <telemetryhub/network/winsock_runtime.hpp>
#include <telemetryhub/protocol/message_codec.hpp>
#include <telemetryhub/protocol/message_framer.hpp>
#include <telemetryhub/protocol/messages.hpp>

#include <array>
#include <chrono>
#include <iomanip>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace device_simulator {
namespace {

using telemetryhub::network::ReceiveStatus;
using telemetryhub::protocol::Command;
using telemetryhub::protocol::ErrorResponse;
using telemetryhub::protocol::MessageFramer;
using telemetryhub::protocol::Request;
using telemetryhub::protocol::RequestId;
using telemetryhub::protocol::Response;

constexpr std::string_view demo_device_id{"demo-sensor-1"};
constexpr std::string_view demo_metric{"temperature"};
constexpr std::chrono::seconds connect_timeout{5};
constexpr std::chrono::seconds response_timeout{5};

class DemoClient
{
public:
    explicit DemoClient(const std::uint16_t port)
    {
        socket_ = telemetryhub::network::create_ipv4_tcp_socket();
        if (const std::error_code error = socket_.set_nonblocking(true))
        {
            throw std::system_error{error, "Failed to configure simulator socket"};
        }
        telemetryhub::network::connect_loopback_until(
            socket_, port, std::chrono::steady_clock::now() + connect_timeout);
    }

    [[nodiscard]] Response request(Command command)
    {
        const RequestId request_id = next_request_id_++;
        const Request request_message{request_id, std::move(command)};
        auto payload = telemetryhub::protocol::serialize_request_payload(request_message);
        if (payload.error.has_value())
        {
            throw std::runtime_error{"Failed to serialize simulator request"};
        }

        auto frame = telemetryhub::protocol::encode_frame(payload.payload);
        if (frame.error.has_value())
        {
            throw std::runtime_error{"Failed to frame simulator request"};
        }

        const auto deadline = std::chrono::steady_clock::now() + response_timeout;
        const auto sent = telemetryhub::network::send_all(socket_, frame.frame, {}, deadline);
        if (sent.error)
        {
            throw std::system_error{sent.error, "Failed to send simulator request"};
        }
        if (sent.timed_out)
        {
            throw std::runtime_error{
                "Timed out sending simulator request " + std::to_string(request_id)};
        }
        if (sent.cancelled || sent.bytes_transferred != frame.frame.size())
        {
            throw std::runtime_error{"Simulator request send did not complete"};
        }

        Response response = receive_response(deadline);
        if (response.request_id != request_id)
        {
            throw std::runtime_error{
                "Response request ID mismatch: expected " + std::to_string(request_id) +
                ", received " + std::to_string(response.request_id)};
        }
        if (const auto* error = std::get_if<ErrorResponse>(&response.body))
        {
            throw std::runtime_error{
                "Server error " + error->code + ": " + error->message};
        }
        return response;
    }

private:
    [[nodiscard]] Response receive_response(
        const std::chrono::steady_clock::time_point deadline)
    {
        std::array<std::byte, 4096U> buffer{};
        for (;;)
        {
            const auto result = telemetryhub::network::receive_some_until(
                socket_, buffer, {}, deadline);
            if (result.status == ReceiveStatus::timed_out)
            {
                throw std::runtime_error{"Timed out waiting for a server response"};
            }
            if (result.status == ReceiveStatus::peer_closed)
            {
                throw std::runtime_error{"Server closed the connection before responding"};
            }
            if (result.status == ReceiveStatus::error)
            {
                throw std::system_error{result.error, "Failed to receive server response"};
            }
            if (result.status == ReceiveStatus::cancelled)
            {
                throw std::runtime_error{"Simulator response receive was cancelled"};
            }

            auto framed = framer_.consume(
                std::span<const std::byte>{buffer.data(), result.bytes_transferred});
            if (framed.error.has_value())
            {
                throw std::runtime_error{"Server sent an invalid response frame"};
            }
            if (framed.payloads.empty())
            {
                continue;
            }
            if (framed.payloads.size() != 1U)
            {
                throw std::runtime_error{"Server sent multiple responses for one request"};
            }

            auto parsed = telemetryhub::protocol::parse_response_payload(framed.payloads.front());
            if (const auto* response = std::get_if<Response>(&parsed))
            {
                return *response;
            }
            if (const auto* json_error =
                    std::get_if<telemetryhub::protocol::JsonInputError>(&parsed))
            {
                throw std::runtime_error{
                    std::string{"Server response contains invalid JSON: "} +
                    telemetryhub::protocol::to_string(json_error->code)};
            }
            const auto& protocol_error =
                std::get<telemetryhub::protocol::ProtocolValidationError>(parsed);
            throw std::runtime_error{
                "Server response violates the protocol: " + protocol_error.message};
        }
    }

    // Winsock must outlive the owned socket; reverse member destruction closes
    // the socket before WinsockRuntime is torn down.
    telemetryhub::network::WinsockRuntime winsock_runtime_;
    telemetryhub::network::Socket socket_;
    MessageFramer framer_;
    RequestId next_request_id_{1U};
};

} // namespace

void run_demo(const std::uint16_t port, std::ostream& output)
{
    DemoClient client{port};
    output << "Connected to 127.0.0.1:" << port << '\n';

    const Response registration = client.request(
        Command{telemetryhub::protocol::RegisterDevice{std::string{demo_device_id}}});
    const auto* registered = std::get_if<telemetryhub::protocol::DeviceRegistered>(
        &registration.body);
    if (registered == nullptr || registered->device_id != demo_device_id)
    {
        throw std::runtime_error{"Server returned an unexpected registration response"};
    }
    output << "Registered device " << demo_device_id << '\n';

    constexpr std::array<double, 3U> readings{21.5, 22.0, 23.5};
    for (const double value : readings)
    {
        const Response response = client.request(
            Command{telemetryhub::protocol::SubmitTelemetry{std::string{demo_metric}, value}});
        if (!std::holds_alternative<telemetryhub::protocol::TelemetryAccepted>(response.body))
        {
            throw std::runtime_error{"Server returned an unexpected telemetry response"};
        }
        output << "Telemetry accepted: " << demo_metric << '=' << std::fixed
               << std::setprecision(2) << value << '\n';
    }

    const Response response = client.request(
        Command{telemetryhub::protocol::GetStatistics{std::string{demo_metric}}});
    const auto* statistics =
        std::get_if<telemetryhub::protocol::Statistics>(&response.body);
    if (statistics == nullptr || statistics->metric != demo_metric)
    {
        throw std::runtime_error{"Server returned an unexpected statistics response"};
    }

    output << "Statistics for " << statistics->metric << ":\n"
           << "  sample_count: " << statistics->sample_count << '\n'
           << "  minimum: " << statistics->minimum << '\n'
           << "  maximum: " << statistics->maximum << '\n'
           << "  mean: " << statistics->mean << '\n'
           << "  last_value: " << statistics->last_value << '\n'
           << "  last_received_at: " << statistics->last_received_at << '\n';
}

} // namespace device_simulator
