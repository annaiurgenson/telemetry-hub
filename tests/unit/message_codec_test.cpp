#include <telemetryhub/protocol/message_codec.hpp>
#include <telemetryhub/protocol/message_framer.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

using namespace telemetryhub::protocol;

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view text)
{
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const unsigned char character : text)
    {
        result.push_back(static_cast<std::byte>(character));
    }
    return result;
}

[[nodiscard]] std::string text(const std::vector<std::byte>& value)
{
    std::string result;
    result.reserve(value.size());
    for (const std::byte character : value)
    {
        result.push_back(static_cast<char>(std::to_integer<unsigned char>(character)));
    }
    return result;
}

[[nodiscard]] Request parse_request_ok(const std::string_view json)
{
    const auto input = bytes(json);
    const auto result = parse_request_payload(input);
    EXPECT_TRUE(std::holds_alternative<Request>(result)) << json;
    if (const auto* request = std::get_if<Request>(&result))
    {
        return *request;
    }
    return {};
}

[[nodiscard]] Response parse_response_ok(const std::string_view json)
{
    const auto input = bytes(json);
    const auto result = parse_response_payload(input);
    EXPECT_TRUE(std::holds_alternative<Response>(result)) << json;
    if (const auto* response = std::get_if<Response>(&result))
    {
        return *response;
    }
    return {};
}

[[nodiscard]] const JsonInputError* json_error(const RequestParseResult& result)
{
    return std::get_if<JsonInputError>(&result);
}

[[nodiscard]] const ProtocolValidationError* protocol_error(const RequestParseResult& result)
{
    return std::get_if<ProtocolValidationError>(&result);
}

[[nodiscard]] const ProtocolValidationError* response_protocol_error(const ResponseParseResult& result)
{
    return std::get_if<ProtocolValidationError>(&result);
}

TEST(MessageCodec, ParsesEveryRequestType)
{
    const Request ping = parse_request_ok(R"({"version":1,"request_id":"1","command":"PING"})");
    EXPECT_EQ(ping.request_id, 1U);
    EXPECT_TRUE(std::holds_alternative<Ping>(ping.command));

    const Request registration = parse_request_ok(
        R"({"version":1,"request_id":"2","command":"REGISTER_DEVICE","device_id":"sensor-1"})");
    ASSERT_TRUE(std::holds_alternative<RegisterDevice>(registration.command));
    EXPECT_EQ(std::get<RegisterDevice>(registration.command).device_id, "sensor-1");

    const Request submission = parse_request_ok(
        R"({"version":1,"request_id":"3","command":"SUBMIT_TELEMETRY","metric":"temperature","value":21.5})");
    ASSERT_TRUE(std::holds_alternative<SubmitTelemetry>(submission.command));
    EXPECT_EQ(std::get<SubmitTelemetry>(submission.command).metric, "temperature");
    EXPECT_DOUBLE_EQ(std::get<SubmitTelemetry>(submission.command).value, 21.5);

    const Request statistics = parse_request_ok(
        R"({"version":1,"request_id":"4","command":"GET_STATISTICS","metric":"temperature"})");
    ASSERT_TRUE(std::holds_alternative<GetStatistics>(statistics.command));
    EXPECT_EQ(std::get<GetStatistics>(statistics.command).metric, "temperature");
}

TEST(MessageCodec, ParsesEveryResponseTypeAndStructuredError)
{
    EXPECT_TRUE(std::holds_alternative<Pong>(parse_response_ok(
        R"({"version":1,"request_id":"10","result":{"type":"PONG"}})").body));
    const Response registered = parse_response_ok(
        R"({"version":1,"request_id":"11","result":{"type":"DEVICE_REGISTERED","device_id":"sensor-1"}})");
    ASSERT_TRUE(std::holds_alternative<DeviceRegistered>(registered.body));
    EXPECT_EQ(std::get<DeviceRegistered>(registered.body).device_id, "sensor-1");
    EXPECT_TRUE(std::holds_alternative<TelemetryAccepted>(parse_response_ok(
        R"({"version":1,"request_id":"12","result":{"type":"TELEMETRY_ACCEPTED"}})").body));
    const Response statistics = parse_response_ok(
        R"({"version":1,"request_id":"13","result":{"type":"STATISTICS","metric":"temperature","sample_count":"18446744073709551615","minimum":-10.0,"maximum":30.0,"mean":5.0,"last_value":12.5,"last_received_at":"2026-10-11T12:30:00.000Z"}})");
    ASSERT_TRUE(std::holds_alternative<Statistics>(statistics.body));
    EXPECT_EQ(std::get<Statistics>(statistics.body).sample_count, std::numeric_limits<std::uint64_t>::max());
    const Response error = parse_response_ok(
        R"({"version":1,"request_id":"14","error":{"code":"SERVER_BUSY","message":"Try again later"}})");
    ASSERT_TRUE(std::holds_alternative<ErrorResponse>(error.body));
    EXPECT_EQ(std::get<ErrorResponse>(error.body).code, "SERVER_BUSY");
}

TEST(MessageCodec, SerializesAndParsesRequestAndResponseRoundTrips)
{
    const std::vector<Request> requests{
        {0U, Ping{}},
        {1U, RegisterDevice{"sensor-1"}},
        {2U, SubmitTelemetry{"temperature", -1.25}},
        {3U, GetStatistics{"temperature"}}};
    for (const Request& request : requests)
    {
        const auto encoded = serialize_request_payload(request);
        ASSERT_FALSE(encoded.error.has_value());
        const auto decoded = parse_request_payload(encoded.payload);
        ASSERT_TRUE(std::holds_alternative<Request>(decoded));
        EXPECT_EQ(std::get<Request>(decoded), request);
    }

    const std::vector<Response> responses{
        {10U, Pong{}},
        {11U, DeviceRegistered{"sensor-1"}},
        {12U, TelemetryAccepted{}},
        {13U, Statistics{"temperature", 5U, 1.0, 9.0, 4.0, 8.0, "2026-10-11T12:30:00.000Z"}},
        {14U, ErrorResponse{"SERVER_BUSY", "Try again later"}}};
    for (const Response& response : responses)
    {
        const auto encoded = serialize_response_payload(response);
        ASSERT_FALSE(encoded.error.has_value());
        const auto decoded = parse_response_payload(encoded.payload);
        ASSERT_TRUE(std::holds_alternative<Response>(decoded));
        EXPECT_EQ(std::get<Response>(decoded), response);
    }
}

TEST(MessageCodec, PreservesCanonicalUint64RequestIdsExactly)
{
    for (const std::string id : {"0", "9007199254740993", "18446744073709551615"})
    {
        const std::string json = "{\"version\":1,\"request_id\":\"" + id + "\",\"command\":\"PING\"}";
        const Request request = parse_request_ok(json);
        const auto encoded = serialize_request_payload(request);
        ASSERT_FALSE(encoded.error.has_value());
        EXPECT_NE(text(encoded.payload).find("\"request_id\":\"" + id + "\""), std::string::npos);
    }
    EXPECT_EQ(parse_request_ok(R"({"version":1,"request_id":"18446744073709551615","command":"PING"})").request_id,
        std::numeric_limits<std::uint64_t>::max());
}

TEST(MessageCodec, EnforcesIdentifierLengthInUtf8Bytes)
{
    const auto parse_device = [](const std::string& device_id) {
        const auto encoded = serialize_request_payload(Request{1U, RegisterDevice{device_id}});
        if (encoded.error.has_value())
        {
            return false;
        }
        return std::holds_alternative<Request>(parse_request_payload(encoded.payload));
    };
    EXPECT_TRUE(parse_device(std::string(max_identifier_size_bytes, 'a')));
    EXPECT_FALSE(parse_device(std::string(max_identifier_size_bytes + 1U, 'a')));

    std::string multibyte_128;
    for (std::size_t index = 0U; index < max_identifier_size_bytes / 2U; ++index)
    {
        multibyte_128.append("\xc3\xa9", 2U);
    }
    EXPECT_TRUE(parse_device(multibyte_128));
    multibyte_128.append("\xc3\xa9", 2U);
    EXPECT_FALSE(parse_device(multibyte_128));
}
TEST(MessageCodec, RejectsNonCanonicalOrOverflowingRequestIdsWithoutCorrelation)
{
    for (const std::string id : {"", "00", "01", "+1", "-1", "1.0", "18446744073709551616"})
    {
        const std::string json = "{\"version\":1,\"request_id\":\"" + id + "\",\"command\":\"PING\"}";
        const auto input = bytes(json);
        const auto result = parse_request_payload(input);
        const auto* error = protocol_error(result);
        ASSERT_NE(error, nullptr) << id;
        EXPECT_EQ(error->code, ProtocolErrorCode::invalid_request_id);
        EXPECT_FALSE(error->request_id.has_value());
    }
}

TEST(MessageCodec, DistinguishesMalformedJsonAndInvalidUtf8)
{
    for (const std::string malformed : {"", "{", "{\"version\":1,}", "{\"version\":1} trailing", "/*x*/{}"})
    {
        const auto input = bytes(malformed);
        const auto result = parse_request_payload(input);
        ASSERT_NE(json_error(result), nullptr) << malformed;
        EXPECT_EQ(json_error(result)->code, malformed.empty() ? JsonInputErrorCode::empty_payload : JsonInputErrorCode::malformed_json);
    }
    const std::vector<std::byte> invalid_utf8{std::byte{'{'}, std::byte{0xffU}, std::byte{'}'}};
    const auto result = parse_request_payload(invalid_utf8);
    ASSERT_NE(json_error(result), nullptr);
    EXPECT_EQ(json_error(result)->code, JsonInputErrorCode::invalid_utf8);
    EXPECT_EQ(json_error(result)->byte_offset, 1U);
}

TEST(MessageCodec, RejectsNulAnywhereIncludingAfterValidJson)
{
    const std::string valid_json = R"({"version":1,"request_id":"1","command":"PING"})";
    auto embedded = bytes(valid_json);
    embedded.insert(embedded.begin() + 10, std::byte{0});
    auto trailing = bytes(valid_json);
    trailing.push_back(std::byte{0});
    trailing.push_back(std::byte{'x'});
    for (const auto& input : {embedded, trailing})
    {
        const auto result = parse_request_payload(input);
        ASSERT_NE(json_error(result), nullptr);
        EXPECT_EQ(json_error(result)->code, JsonInputErrorCode::embedded_nul);
    }
}
TEST(MessageCodec, RejectsDuplicateKeysAtEveryObjectDepth)
{
    for (const std::string json : {
             R"({"version":1,"version":1,"request_id":"1","command":"PING"})",
             R"({"version":1,"request_id":"1","command":"PING","extra":{"x":1,"\u0078":2}})",
             R"({"version":1,"request_id":"1","command":"PING","extra":{"x":1,"x":2}})"})
    {
        const auto input = bytes(json);
        const auto result = parse_request_payload(input);
        ASSERT_NE(json_error(result), nullptr);
        EXPECT_EQ(json_error(result)->code, JsonInputErrorCode::duplicate_key);
    }
}

TEST(MessageCodec, RejectsUnknownMissingAndIncorrectlyTypedFields)
{
    for (const std::string json : {
             R"({"version":1,"request_id":"1","command":"PING","extra":true})",
             R"({"version":1,"request_id":"1","command":"REGISTER_DEVICE"})"})
    {
        const auto input = bytes(json);
        const auto result = parse_request_payload(input);
        ASSERT_NE(protocol_error(result), nullptr);
        EXPECT_EQ(protocol_error(result)->request_id, 1U);
    }
    const auto missing = parse_request_payload(bytes(R"({"version":1,"request_id":"1"})"));
    ASSERT_NE(protocol_error(missing), nullptr);
    EXPECT_EQ(protocol_error(missing)->code, ProtocolErrorCode::missing_field);
    const auto bad_version = parse_request_payload(bytes(R"({"version":1.0,"request_id":"1","command":"PING"})"));
    ASSERT_NE(protocol_error(bad_version), nullptr);
    EXPECT_EQ(protocol_error(bad_version)->code, ProtocolErrorCode::invalid_field_type);
    const auto wrong_type = parse_request_payload(bytes(R"({"version":1,"request_id":"1","command":9})"));
    ASSERT_NE(protocol_error(wrong_type), nullptr);
    EXPECT_EQ(protocol_error(wrong_type)->code, ProtocolErrorCode::invalid_field_type);
    const auto unknown_command = parse_request_payload(bytes(R"({"version":1,"request_id":"1","command":"NOPE"})"));
    ASSERT_NE(protocol_error(unknown_command), nullptr);
    EXPECT_EQ(protocol_error(unknown_command)->code, ProtocolErrorCode::unknown_command);
    EXPECT_EQ(protocol_error(unknown_command)->request_id, 1U);
    EXPECT_FALSE(protocol_error(unknown_command)->message.empty());
}

TEST(MessageCodec, EnforcesPayloadBoundaryOnDirectCodecCalls)
{
    std::string at_limit = R"({"version":1,"request_id":"1","command":"PING"})";
    at_limit.append(max_payload_size_bytes - at_limit.size(), ' ');
    const auto accepted = parse_request_payload(bytes(at_limit));
    EXPECT_TRUE(std::holds_alternative<Request>(accepted));
    at_limit.push_back(' ');
    const auto rejected = parse_request_payload(bytes(at_limit));
    ASSERT_NE(json_error(rejected), nullptr);
    EXPECT_EQ(json_error(rejected)->code, JsonInputErrorCode::payload_too_large);
}

TEST(MessageCodec, EnforcesNestingAndObjectMemberLimitsDuringParse)
{
    std::string at_depth_limit = R"({"version":1,"request_id":"1","command":"PING","x":)";
    at_depth_limit.append(max_json_nesting_depth - 1U, '[');
    at_depth_limit.append(max_json_nesting_depth - 1U, ']');
    at_depth_limit.push_back('}');
    EXPECT_NE(protocol_error(parse_request_payload(bytes(at_depth_limit))), nullptr);

    std::string too_deep = R"({"version":1,"request_id":"1","command":"PING","x":)";
    too_deep.append(max_json_nesting_depth, '[');
    too_deep.append(max_json_nesting_depth, ']');
    too_deep.push_back('}');
    const auto deep_result = parse_request_payload(bytes(too_deep));
    ASSERT_NE(json_error(deep_result), nullptr);
    EXPECT_EQ(json_error(deep_result)->code, JsonInputErrorCode::nesting_too_deep);

    std::string at_member_limit = R"({"version":1,"request_id":"1","command":"PING")";
    for (std::size_t index = 0U; index < max_json_object_members - 3U; ++index)
    {
        at_member_limit += ",\"extra" + std::to_string(index) + "\":0";
    }
    at_member_limit.push_back('}');
    EXPECT_NE(protocol_error(parse_request_payload(bytes(at_member_limit))), nullptr);
    at_member_limit.insert(at_member_limit.size() - 1U, ",\"overflow\":0");
    const auto over_limit = parse_request_payload(bytes(at_member_limit));
    ASSERT_NE(json_error(over_limit), nullptr);
    EXPECT_EQ(json_error(over_limit)->code, JsonInputErrorCode::too_many_object_members);
}
TEST(MessageCodec, ValidatesResponseShapeAndExactlyOneOfResultOrError)
{
    for (const std::string json : {
             R"({"version":1,"request_id":"9","result":{"type":"PONG"},"error":{"code":"X","message":"x"}})",
             R"({"version":1,"request_id":"9"})",
             R"({"version":1,"request_id":"9","result":{"type":"PONG","ok":true}})"})
    {
        const auto result = parse_response_payload(bytes(json));
        ASSERT_NE(response_protocol_error(result), nullptr);
    }
}

TEST(MessageCodec, ValidatesNumericRangesAndFiniteStatistics)
{
    for (const std::string number : {"1e400", "NaN", "Infinity"})
    {
        const auto result = parse_request_payload(bytes(
            "{\"version\":1,\"request_id\":\"1\",\"command\":\"SUBMIT_TELEMETRY\",\"metric\":\"m\",\"value\":" + number + "}"));
        EXPECT_FALSE(std::holds_alternative<Request>(result)) << number;
    }
    const auto valid = parse_request_payload(bytes(
        R"({"version":1,"request_id":"1","command":"SUBMIT_TELEMETRY","metric":"m","value":1.7976931348623157e308})"));
    ASSERT_TRUE(std::holds_alternative<Request>(valid));
    const auto invalid_stats = parse_response_payload(bytes(
        R"({"version":1,"request_id":"1","result":{"type":"STATISTICS","metric":"m","sample_count":"1","minimum":"3","maximum":4,"mean":3.5,"last_value":2,"last_received_at":"2026-10-11T12:30:00.000Z"}})"));
    ASSERT_NE(response_protocol_error(invalid_stats), nullptr);
    const auto invalid_timestamp = parse_response_payload(bytes(
        R"({"version":1,"request_id":"1","result":{"type":"STATISTICS","metric":"m","sample_count":"1","minimum":1,"maximum":2,"mean":1.5,"last_value":2,"last_received_at":"not-a-timestamp"}})"));
    ASSERT_NE(response_protocol_error(invalid_timestamp), nullptr);
}

TEST(MessageCodec, RejectsInvalidOutgoingTypedMessages)
{
    EXPECT_EQ(serialize_request_payload(Request{1U, RegisterDevice{std::string(129U, 'a')}}).error->code,
        SerializationErrorCode::invalid_message);
    EXPECT_EQ(serialize_request_payload(Request{2U, SubmitTelemetry{"m", std::numeric_limits<double>::infinity()}})
                  .error->code,
        SerializationErrorCode::invalid_message);
    EXPECT_EQ(serialize_request_payload(Request{3U, RegisterDevice{std::string{"bad\xff", 4U}}}).error->code,
        SerializationErrorCode::invalid_utf8);
    EXPECT_EQ(serialize_response_payload(Response{4U, Statistics{"m", 1U, 1.0, 2.0, std::numeric_limits<double>::quiet_NaN(), 1.0, "2026-10-11T12:30:00.000Z"}})
                  .error->code,
        SerializationErrorCode::invalid_message);
    EXPECT_EQ(serialize_response_payload(Response{5U, ErrorResponse{"lowercase", "bad"}}).error->code,
        SerializationErrorCode::invalid_message);
    const auto oversized_error = serialize_response_payload(
        Response{6U, ErrorResponse{"SERVER_BUSY", std::string(max_payload_size_bytes + 1U, 'x')}});
    ASSERT_TRUE(oversized_error.error.has_value());
    EXPECT_EQ(oversized_error.error->code, SerializationErrorCode::payload_too_large);
}

TEST(MessageCodec, UsesStableErrorCodesAndOnlyCarriesTrustworthyCorrelation)
{
    EXPECT_STREQ(to_string(JsonInputErrorCode::duplicate_key), "DUPLICATE_KEY");
    EXPECT_STREQ(to_string(ProtocolErrorCode::unknown_command), "UNKNOWN_COMMAND");
    const auto known = parse_request_payload(bytes(R"({"version":1,"request_id":"7","command":"UNKNOWN"})"));
    ASSERT_NE(protocol_error(known), nullptr);
    EXPECT_EQ(protocol_error(known)->request_id, 7U);
    const auto impossible = parse_request_payload(bytes(R"({"version":1,"request_id":7,"command":"PING"})"));
    ASSERT_NE(protocol_error(impossible), nullptr);
    EXPECT_FALSE(protocol_error(impossible)->request_id.has_value());
}

TEST(MessageCodec, KeepsJsonLibraryOutOfPublicHeadersAndReusesFramingPayload)
{
    const Request request{55U, Ping{}};
    const auto payload = serialize_request_payload(request);
    ASSERT_FALSE(payload.error.has_value());
    const auto frame = encode_frame(payload.payload);
    ASSERT_FALSE(frame.error.has_value());
    EXPECT_EQ(frame.frame.size(), frame_header_size_bytes + payload.payload.size());
    static_assert(std::is_same_v<RequestId, std::uint64_t>);
}

TEST(MessageCodec, RoundTripsStatisticsNotFoundApplicationError)
{
    const Response response{91U, ErrorResponse{"STATISTICS_NOT_FOUND", "No samples are available"}};
    const auto encoded = serialize_response_payload(response);
    ASSERT_FALSE(encoded.error.has_value());

    const auto parsed = parse_response_payload(encoded.payload);
    const auto* decoded = std::get_if<Response>(&parsed);
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(*decoded, response);
}
} // namespace
