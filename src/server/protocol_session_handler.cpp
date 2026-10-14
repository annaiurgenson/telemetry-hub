#include <telemetryhub/server/protocol_session_handler.hpp>

#include <telemetryhub/protocol/message_codec.hpp>
#include <telemetryhub/protocol/message_framer.hpp>

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace telemetryhub::server {
namespace {

using network::ConnectionId;
using network::OutboundAdmission;
using network::SessionCloseMode;
using network::SessionOutbound;
using network::SessionOutput;
using protocol::ErrorResponse;
using protocol::Response;
using protocol::ResponseBody;

struct ProtocolConnectionState
{
    ConnectionId connection_id;
    std::shared_ptr<DeviceRegistrationRegistry> registrations;
    std::shared_ptr<CommandIngress> ingress;
    protocol::MessageFramer framer;
};

[[nodiscard]] bool append_response(
    SessionOutput& output,
    const Response& response,
    const OutboundAdmission admission = OutboundAdmission::wait_for_capacity)
{
    auto payload = protocol::serialize_response_payload(response);
    if (payload.error.has_value())
    {
        return false;
    }
    auto frame = protocol::encode_frame(payload.payload);
    if (frame.error.has_value())
    {
        return false;
    }
    output.outbound.push_back(SessionOutbound{std::move(frame.frame), admission});
    return true;
}

[[nodiscard]] Response error_response(
    const protocol::RequestId request_id,
    std::string code,
    std::string message)
{
    return Response{request_id, ResponseBody{ErrorResponse{std::move(code), std::move(message)}}};
}

[[nodiscard]] SessionOutput handle_bytes(
    const std::shared_ptr<ProtocolConnectionState>& state,
    const std::span<const std::byte> bytes)
{
    SessionOutput output;
    const auto framed = state->framer.consume(bytes);
    bool terminal = false;

    for (const auto& payload : framed.payloads)
    {
        auto parsed = protocol::parse_request_payload(payload);
        if (const auto* request = std::get_if<protocol::Request>(&parsed))
        {
            Response response{request->request_id, ResponseBody{protocol::Pong{}}};
            bool queued_for_processing = false;
            bool ingress_closed = false;
            std::visit(
                [&response, &state, request_id = request->request_id,
                 connection_id = state->connection_id, &queued_for_processing, &ingress_closed](const auto& command) {
                    using Command = std::decay_t<decltype(command)>;
                    if constexpr (std::is_same_v<Command, protocol::Ping>)
                    {
                        response.body = protocol::Pong{};
                    }
                    else if constexpr (std::is_same_v<Command, protocol::RegisterDevice>)
                    {
                        const auto result = state->registrations->register_device(
                            connection_id, command.device_id);
                        if (result == RegistrationResult::registered ||
                            result == RegistrationResult::already_registered)
                        {
                            response.body = protocol::DeviceRegistered{command.device_id};
                        }
                        else if (result == RegistrationResult::connection_already_registered)
                        {
                            response.body = ErrorResponse{
                                "ALREADY_REGISTERED", "This connection already owns a different device"};
                        }
                        else
                        {
                            response.body = ErrorResponse{
                                "DEVICE_IN_USE", "The device is already registered to another connection"};
                        }
                    }
                    else if constexpr (std::is_same_v<Command, protocol::SubmitTelemetry> ||
                                       std::is_same_v<Command, protocol::GetStatistics>)
                    {
                        const auto device_id = state->registrations->device_for_connection(connection_id);
                        if (!device_id.has_value())
                        {
                            response.body = ErrorResponse{
                                "DEVICE_NOT_REGISTERED", "Register a device before issuing this command"};
                            return;
                        }

                        ProcessingCommand processing_command{command};
                        const CommandAdmission admission = state->ingress->submit(CommandEnvelope{
                            connection_id, request_id, *device_id, std::move(processing_command)});
                        switch (admission)
                        {
                        case CommandAdmission::accepted:
                            queued_for_processing = true;
                            break;
                        case CommandAdmission::full:
                            response.body = ErrorResponse{
                                "SERVER_BUSY", "The server is temporarily unable to accept more commands"};
                            break;
                        case CommandAdmission::closed:
                            ingress_closed = true;
                            break;
                        }
                    }
                },
                request->command);

            if (ingress_closed)
            {
                output.close_mode = SessionCloseMode::close_now;
                return output;
            }
            if (queued_for_processing)
            {
                continue;
            }
            const OutboundAdmission admission =
                std::holds_alternative<ErrorResponse>(response.body) &&
                        std::get<ErrorResponse>(response.body).code == "SERVER_BUSY"
                    ? OutboundAdmission::try_once
                    : OutboundAdmission::wait_for_capacity;
            if (!append_response(output, response, admission))
            {
                output.close_mode = SessionCloseMode::close_now;
                return output;
            }
            continue;
        }

        if (const auto* error = std::get_if<protocol::ProtocolValidationError>(&parsed))
        {
            if (!error->request_id.has_value())
            {
                terminal = true;
                break;
            }
            const std::string message = error->message.empty()
                                            ? "The request does not match the supported protocol"
                                            : error->message;
            if (!append_response(
                    output,
                    error_response(*error->request_id, protocol::to_string(error->code), message),
                    OutboundAdmission::try_once))
            {
                output.close_mode = SessionCloseMode::close_now;
                return output;
            }
            terminal = true;
            break;
        }

        terminal = true;
        break;
    }

    if (framed.error.has_value())
    {
        terminal = true;
    }

    if (terminal)
    {
        // Keep all earlier complete-frame responses bounded if this read later
        // contains fatal input; preserve their order in the session writer.
        for (auto& item : output.outbound)
        {
            item.admission = OutboundAdmission::try_once;
        }
        output.close_mode = !output.outbound.empty()
                                ? SessionCloseMode::close_after_flush
                                : SessionCloseMode::close_now;
    }
    return output;
}

} // namespace

ProtocolSessionHandler::ProtocolSessionHandler(
    std::shared_ptr<DeviceRegistrationRegistry> registrations,
    std::shared_ptr<CommandIngress> ingress)
    : registrations_{std::move(registrations)},
      ingress_{std::move(ingress)}
{
    if (!registrations_ || !ingress_)
    {
        throw std::invalid_argument{"ProtocolSessionHandler requires a registry and command ingress"};
    }
}

network::SessionCallbacks ProtocolSessionHandler::callbacks_for(const ConnectionId connection_id) const
{
    auto state = std::make_shared<ProtocolConnectionState>(
        ProtocolConnectionState{connection_id, registrations_, ingress_, {}});
    const auto registrations = registrations_;
    return network::SessionCallbacks{
        [state](const ConnectionId callback_id, const std::span<const std::byte> bytes) {
            if (callback_id != state->connection_id)
            {
                return SessionOutput{{}, SessionCloseMode::close_now};
            }
            return handle_bytes(state, bytes);
        },
        [registrations, connection_id](const ConnectionId disconnected_id) {
            if (disconnected_id == connection_id)
            {
                registrations->erase_connection(disconnected_id);
            }
        }};
}

std::shared_ptr<DeviceRegistrationRegistry> ProtocolSessionHandler::registrations() const noexcept
{
    return registrations_;
}

} // namespace telemetryhub::server
