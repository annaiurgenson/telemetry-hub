#pragma once

#include <telemetryhub/network/tcp_server.hpp>
#include <telemetryhub/protocol/messages.hpp>

#include <cstdint>
#include <string>
#include <variant>

namespace telemetryhub::server {

using ProcessingCommand = std::variant<protocol::SubmitTelemetry, protocol::GetStatistics>;

// Owns all request data needed after the reader callback returns. It contains
// no socket, session pointer, or borrowed parser storage.
struct CommandEnvelope
{
    network::ConnectionId connection_id;
    protocol::RequestId request_id{};
    std::string device_id;
    ProcessingCommand command;
};

} // namespace telemetryhub::server
