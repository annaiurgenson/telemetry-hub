#pragma once

#include <telemetryhub/domain/telemetry_store.hpp>
#include <telemetryhub/network/tcp_server.hpp>
#include <telemetryhub/protocol/messages.hpp>

#include <cstdint>
#include <string>
#include <variant>

namespace telemetryhub::server {

using ProcessingCommand = std::variant<protocol::SubmitTelemetry, protocol::GetStatistics>;

// Owns all request data needed after the reader callback returns. It contains
// no socket, session pointer, or borrowed parser storage. CommandIngress assigns
// a nonzero receive_sequence before attempting queue admission.
struct CommandEnvelope
{
    network::ConnectionId connection_id;
    protocol::RequestId request_id{};
    std::string device_id;
    ProcessingCommand command;
    domain::ReceivedAt received_at{};
    domain::ReceiveSequence receive_sequence{};
};

} // namespace telemetryhub::server
