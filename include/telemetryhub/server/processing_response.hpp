#pragma once

#include <telemetryhub/domain/telemetry_processor.hpp>
#include <telemetryhub/protocol/messages.hpp>
#include <telemetryhub/server/command_envelope.hpp>

#include <optional>
#include <string>

namespace telemetryhub::server {

[[nodiscard]] std::optional<std::string> format_received_timestamp(
    domain::ReceivedAt received_at);

[[nodiscard]] protocol::Response make_processing_response(
    const CommandEnvelope& envelope,
    const domain::TelemetryProcessor& processor);

[[nodiscard]] protocol::Response make_processing_unavailable_response(
    protocol::RequestId request_id);

} // namespace telemetryhub::server
