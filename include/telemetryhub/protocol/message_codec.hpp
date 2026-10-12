#pragma once

#include <telemetryhub/protocol/messages.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace telemetryhub::protocol {

enum class JsonInputErrorCode
{
    empty_payload,
    payload_too_large,
    embedded_nul,
    invalid_utf8,
    malformed_json,
    duplicate_key,
    nesting_too_deep,
    too_many_object_members
};

struct JsonInputError
{
    JsonInputErrorCode code{};
    std::optional<std::size_t> byte_offset;
};

enum class ProtocolErrorCode
{
    unsupported_version,
    invalid_root_type,
    missing_field,
    unknown_field,
    invalid_field_type,
    invalid_field_value,
    invalid_request_id,
    unknown_command,
    invalid_response_shape
};

struct ProtocolValidationError
{
    ProtocolErrorCode code{};
    std::optional<RequestId> request_id;
    std::string field;
    std::string message;
};

using RequestParseResult = std::variant<Request, JsonInputError, ProtocolValidationError>;
using ResponseParseResult = std::variant<Response, JsonInputError, ProtocolValidationError>;

enum class SerializationErrorCode
{
    invalid_message,
    invalid_utf8,
    payload_too_large
};

struct SerializationError
{
    SerializationErrorCode code{};
    std::optional<RequestId> request_id;
    std::string field;
};

struct PayloadSerializationResult
{
    std::vector<std::byte> payload;
    std::optional<SerializationError> error;
};

// Parses one complete UTF-8 JSON payload. JSON syntax/resource failures are
// reported separately from protocol validation failures. A validation error
// carries request_id only when it was a valid canonical uint64 decimal string.
[[nodiscard]] RequestParseResult parse_request_payload(std::span<const std::byte> payload);
[[nodiscard]] ResponseParseResult parse_response_payload(std::span<const std::byte> payload);

// Serializes one validated typed message to a UTF-8 JSON payload (without the
// four-byte transport frame). The payload is bounded by max_payload_size_bytes.
[[nodiscard]] PayloadSerializationResult serialize_request_payload(const Request& request);
[[nodiscard]] PayloadSerializationResult serialize_response_payload(const Response& response);

[[nodiscard]] const char* to_string(JsonInputErrorCode code) noexcept;
[[nodiscard]] const char* to_string(ProtocolErrorCode code) noexcept;
[[nodiscard]] const char* to_string(SerializationErrorCode code) noexcept;

} // namespace telemetryhub::protocol
