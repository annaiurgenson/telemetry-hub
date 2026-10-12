#include <telemetryhub/protocol/message_codec.hpp>

#include <telemetryhub/protocol/message_framer.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace telemetryhub::protocol {
namespace {

using Json = nlohmann::json;

struct ParseLimitSignal
{
    JsonInputErrorCode code;
};

struct ParseGuard
{
    std::vector<std::set<std::string, std::less<>>> object_keys;
};

[[nodiscard]] bool is_valid_utf8(const std::string_view text, std::size_t* invalid_offset = nullptr) noexcept
{
    const auto* const bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0U;
    const auto fail = [&](const std::size_t at) {
        if (invalid_offset != nullptr)
        {
            *invalid_offset = at;
        }
        return false;
    };

    while (index < text.size())
    {
        const unsigned char first = bytes[index];
        if (first <= 0x7fU)
        {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0U;
        unsigned char second_min = 0x80U;
        unsigned char second_max = 0xbfU;
        if (first >= 0xc2U && first <= 0xdfU)
        {
            continuation_count = 1U;
        }
        else if (first >= 0xe0U && first <= 0xefU)
        {
            continuation_count = 2U;
            if (first == 0xe0U)
            {
                second_min = 0xa0U;
            }
            else if (first == 0xedU)
            {
                second_max = 0x9fU;
            }
        }
        else if (first >= 0xf0U && first <= 0xf4U)
        {
            continuation_count = 3U;
            if (first == 0xf0U)
            {
                second_min = 0x90U;
            }
            else if (first == 0xf4U)
            {
                second_max = 0x8fU;
            }
        }
        else
        {
            return fail(index);
        }

        if (continuation_count > text.size() - index - 1U)
        {
            return fail(index);
        }
        const unsigned char second = bytes[index + 1U];
        if (second < second_min || second > second_max)
        {
            return fail(index + 1U);
        }
        for (std::size_t offset = 2U; offset <= continuation_count; ++offset)
        {
            const unsigned char continuation = bytes[index + offset];
            if (continuation < 0x80U || continuation > 0xbfU)
            {
                return fail(index + offset);
            }
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] bool contains_nul(const std::string_view text) noexcept
{
    return text.find('\0') != std::string_view::npos;
}

[[nodiscard]] std::optional<JsonInputError> validate_payload_bytes(const std::span<const std::byte> payload)
{
    if (payload.empty())
    {
        return JsonInputError{JsonInputErrorCode::empty_payload, std::nullopt};
    }
    if (payload.size() > max_payload_size_bytes)
    {
        return JsonInputError{JsonInputErrorCode::payload_too_large, std::nullopt};
    }
    for (std::size_t index = 0U; index < payload.size(); ++index)
    {
        if (payload[index] == std::byte{0})
        {
            return JsonInputError{JsonInputErrorCode::embedded_nul, index};
        }
    }

    const std::string_view text{reinterpret_cast<const char*>(payload.data()), payload.size()};
    std::size_t invalid_utf8_offset = 0U;
    if (!is_valid_utf8(text, &invalid_utf8_offset))
    {
        return JsonInputError{JsonInputErrorCode::invalid_utf8, invalid_utf8_offset};
    }
    return std::nullopt;
}
struct ParsedInput
{
    Json value;
    std::optional<JsonInputError> error;
};

[[nodiscard]] ParsedInput parse_json_payload(const std::span<const std::byte> payload)
{
    const auto precheck = validate_payload_bytes(payload);
    if (precheck.has_value())
    {
        return {{}, *precheck};
    }

    const std::string_view text{reinterpret_cast<const char*>(payload.data()), payload.size()};
    ParseGuard guard;
    const auto callback = [&guard](const int depth, const Json::parse_event_t event, Json& parsed) -> bool {
        if ((event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) &&
            static_cast<std::size_t>(depth) + 1U > max_json_nesting_depth)
        {
            throw ParseLimitSignal{JsonInputErrorCode::nesting_too_deep};
        }
        if (event == Json::parse_event_t::object_start)
        {
            guard.object_keys.emplace_back();
        }
        else if (event == Json::parse_event_t::key)
        {
            if (guard.object_keys.empty())
            {
                throw ParseLimitSignal{JsonInputErrorCode::malformed_json};
            }
            const auto& key = parsed.get_ref<const std::string&>();
            auto& keys = guard.object_keys.back();
            if (!keys.emplace(key).second)
            {
                throw ParseLimitSignal{JsonInputErrorCode::duplicate_key};
            }
            if (keys.size() > max_json_object_members)
            {
                throw ParseLimitSignal{JsonInputErrorCode::too_many_object_members};
            }
        }
        else if (event == Json::parse_event_t::object_end)
        {
            if (!guard.object_keys.empty())
            {
                guard.object_keys.pop_back();
            }
        }
        return true;
    };

    try
    {
        Json value = Json::parse(text.begin(), text.end(), callback, true, false);
        return {std::move(value), std::nullopt};
    }
    catch (const ParseLimitSignal& signal)
    {
        return {{}, JsonInputError{signal.code, std::nullopt}};
    }
    catch (const Json::parse_error& error)
    {
        std::optional<std::size_t> position;
        if (error.byte > 0U)
        {
            position = error.byte - 1U;
        }
        return {{}, JsonInputError{JsonInputErrorCode::malformed_json, position}};
    }
    catch (const Json::exception&)
    {
        return {{}, JsonInputError{JsonInputErrorCode::malformed_json, std::nullopt}};
    }
}

[[nodiscard]] std::optional<RequestId> parse_canonical_uint64(const Json& value)
{
    if (!value.is_string())
    {
        return std::nullopt;
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || (text.size() > 1U && text.front() == '0'))
    {
        return std::nullopt;
    }
    for (const char character : text)
    {
        if (character < '0' || character > '9')
        {
            return std::nullopt;
        }
    }
    RequestId result = 0U;
    const auto conversion = std::from_chars(text.data(), text.data() + text.size(), result);
    if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size())
    {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] bool is_integer_one(const Json& value)
{
    if (!value.is_number_integer())
    {
        return false;
    }
    if (value.is_number_unsigned())
    {
        return value.get<std::uint64_t>() == protocol_version;
    }
    return value.get<std::int64_t>() == static_cast<std::int64_t>(protocol_version);
}

[[nodiscard]] ProtocolValidationError validation_error(const ProtocolErrorCode code,
    const std::optional<RequestId> request_id, std::string field = {})
{
    std::string message;
    switch (code)
    {
    case ProtocolErrorCode::unsupported_version: message = "Unsupported protocol version"; break;
    case ProtocolErrorCode::invalid_root_type: message = "The message must be a JSON object"; break;
    case ProtocolErrorCode::missing_field: message = "A required field is missing"; break;
    case ProtocolErrorCode::unknown_field: message = "The message contains an unknown field"; break;
    case ProtocolErrorCode::invalid_field_type: message = "A field has an invalid type"; break;
    case ProtocolErrorCode::invalid_field_value: message = "A field has an invalid value"; break;
    case ProtocolErrorCode::invalid_request_id: message = "request_id must be a canonical uint64 decimal string"; break;
    case ProtocolErrorCode::unknown_command: message = "The command is not supported"; break;
    case ProtocolErrorCode::invalid_response_shape: message = "A response must contain exactly one of result or error"; break;
    }
    if (!field.empty())
    {
        message += ": ";
        message += field;
    }
    return {code, request_id, std::move(field), std::move(message)};
}

[[nodiscard]] bool has_only_fields(const Json& object, const std::initializer_list<std::string_view> fields)
{
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator)
    {
        if (std::find(fields.begin(), fields.end(), iterator.key()) == fields.end())
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool has_field(const Json& object, const std::string_view field)
{
    return object.find(field) != object.end();
}

[[nodiscard]] std::optional<ProtocolValidationError> validate_root_fields(
    const Json& root, const std::optional<RequestId> request_id,
    const std::initializer_list<std::string_view> allowed_fields)
{
    if (!has_only_fields(root, allowed_fields))
    {
        return validation_error(ProtocolErrorCode::unknown_field, request_id);
    }
    for (const auto field : allowed_fields)
    {
        if (!has_field(root, field))
        {
            return validation_error(ProtocolErrorCode::missing_field, request_id, std::string{field});
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool is_valid_identifier(const std::string_view value) noexcept
{
    if (value.empty() || value.size() > max_identifier_size_bytes || contains_nul(value))
    {
        return false;
    }
    for (const unsigned char character : value)
    {
        if (character < 0x20U || character == 0x7fU)
        {
            return false;
        }
    }
    return is_valid_utf8(value);
}

[[nodiscard]] bool is_valid_code(const std::string_view value) noexcept
{
    if (value.empty())
    {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_';
    });
}

[[nodiscard]] bool is_valid_message(const std::string_view value) noexcept
{
    return !value.empty() && !contains_nul(value) && is_valid_utf8(value);
}

[[nodiscard]] bool is_valid_received_timestamp(const std::string_view value) noexcept
{
    if (value.size() != 24U || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
        value[16] != ':' || value[19] != '.' || value[23] != 'Z')
    {
        return false;
    }
    const auto number_at = [value](const std::size_t offset, const std::size_t length) noexcept {
        int result = 0;
        for (std::size_t index = offset; index < offset + length; ++index)
        {
            const char character = value[index];
            if (character < '0' || character > '9')
            {
                return -1;
            }
            result = result * 10 + (character - '0');
        }
        return result;
    };
    const int year = number_at(0U, 4U);
    const int month = number_at(5U, 2U);
    const int day = number_at(8U, 2U);
    const int hour = number_at(11U, 2U);
    const int minute = number_at(14U, 2U);
    const int second = number_at(17U, 2U);
    if (year < 0 || month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59 || number_at(20U, 3U) < 0)
    {
        return false;
    }
    const std::chrono::year_month_day date{std::chrono::year{year},
        std::chrono::month{static_cast<unsigned int>(month)}, std::chrono::day{static_cast<unsigned int>(day)}};
    return date.ok();
}
[[nodiscard]] bool is_finite(const double value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool parse_number(const Json& object, const std::string_view field, double& output)
{
    if (!has_field(object, field) || !object.at(field).is_number())
    {
        return false;
    }
    output = object.at(field).get<double>();
    return is_finite(output);
}

[[nodiscard]] bool parse_string(const Json& object, const std::string_view field, std::string& output)
{
    if (!has_field(object, field) || !object.at(field).is_string())
    {
        return false;
    }
    output = object.at(field).get<std::string>();
    return true;
}

[[nodiscard]] std::optional<ProtocolValidationError> validate_version_and_id(
    const Json& root, std::optional<RequestId>& request_id)
{
    const auto id_iterator = root.find("request_id");
    if (id_iterator != root.end())
    {
        request_id = parse_canonical_uint64(*id_iterator);
        if (!request_id.has_value())
        {
            return validation_error(ProtocolErrorCode::invalid_request_id, std::nullopt, "request_id");
        }
    }
    else
    {
        return validation_error(ProtocolErrorCode::missing_field, std::nullopt, "request_id");
    }

    const auto version_iterator = root.find("version");
    if (version_iterator == root.end())
    {
        return validation_error(ProtocolErrorCode::missing_field, request_id, "version");
    }
    if (!version_iterator->is_number_integer())
    {
        return validation_error(ProtocolErrorCode::invalid_field_type, request_id, "version");
    }
    if (!is_integer_one(*version_iterator))
    {
        return validation_error(ProtocolErrorCode::unsupported_version, request_id, "version");
    }
    return std::nullopt;
}

[[nodiscard]] RequestParseResult parse_request(const Json& root)
{
    if (!root.is_object())
    {
        return validation_error(ProtocolErrorCode::invalid_root_type, std::nullopt);
    }

    std::optional<RequestId> request_id;
    if (const auto error = validate_version_and_id(root, request_id); error.has_value())
    {
        return *error;
    }

    const auto command_iterator = root.find("command");
    if (command_iterator == root.end())
    {
        return validation_error(ProtocolErrorCode::missing_field, request_id, "command");
    }
    if (!command_iterator->is_string())
    {
        return validation_error(ProtocolErrorCode::invalid_field_type, request_id, "command");
    }
    const std::string command = command_iterator->get<std::string>();

    if (command == "PING")
    {
        if (const auto error = validate_root_fields(root, request_id, {"version", "request_id", "command"}); error)
        {
            return *error;
        }
        return Request{*request_id, Ping{}};
    }
    if (command == "REGISTER_DEVICE")
    {
        if (const auto error = validate_root_fields(
                root, request_id, {"version", "request_id", "command", "device_id"}); error)
        {
            return *error;
        }
        std::string device_id;
        if (!parse_string(root, "device_id", device_id))
        {
            return validation_error(ProtocolErrorCode::invalid_field_type, request_id, "device_id");
        }
        if (!is_valid_identifier(device_id))
        {
            return validation_error(ProtocolErrorCode::invalid_field_value, request_id, "device_id");
        }
        return Request{*request_id, RegisterDevice{std::move(device_id)}};
    }
    if (command == "SUBMIT_TELEMETRY")
    {
        if (const auto error = validate_root_fields(
                root, request_id, {"version", "request_id", "command", "metric", "value"}); error)
        {
            return *error;
        }
        std::string metric;
        if (!parse_string(root, "metric", metric))
        {
            return validation_error(ProtocolErrorCode::invalid_field_type, request_id, "metric");
        }
        if (!is_valid_identifier(metric))
        {
            return validation_error(ProtocolErrorCode::invalid_field_value, request_id, "metric");
        }
        double value = 0.0;
        if (!parse_number(root, "value", value))
        {
            return validation_error(ProtocolErrorCode::invalid_field_value, request_id, "value");
        }
        return Request{*request_id, SubmitTelemetry{std::move(metric), value}};
    }
    if (command == "GET_STATISTICS")
    {
        if (const auto error = validate_root_fields(
                root, request_id, {"version", "request_id", "command", "metric"}); error)
        {
            return *error;
        }
        std::string metric;
        if (!parse_string(root, "metric", metric))
        {
            return validation_error(ProtocolErrorCode::invalid_field_type, request_id, "metric");
        }
        if (!is_valid_identifier(metric))
        {
            return validation_error(ProtocolErrorCode::invalid_field_value, request_id, "metric");
        }
        return Request{*request_id, GetStatistics{std::move(metric)}};
    }
    return validation_error(ProtocolErrorCode::unknown_command, request_id, "command");
}

[[nodiscard]] ProtocolValidationError invalid_response(
    const ProtocolErrorCode code, const std::optional<RequestId> request_id, std::string field = {})
{
    return validation_error(code, request_id, std::move(field));
}

[[nodiscard]] ResponseParseResult parse_response(const Json& root)
{
    if (!root.is_object())
    {
        return invalid_response(ProtocolErrorCode::invalid_root_type, std::nullopt);
    }
    std::optional<RequestId> request_id;
    if (const auto error = validate_version_and_id(root, request_id); error.has_value())
    {
        return *error;
    }
    const bool has_result = has_field(root, "result");
    const bool has_error = has_field(root, "error");
    if (has_result == has_error)
    {
        return invalid_response(ProtocolErrorCode::invalid_response_shape, request_id);
    }
    if (!has_only_fields(root, {"version", "request_id", "result", "error"}))
    {
        return invalid_response(ProtocolErrorCode::unknown_field, request_id);
    }

    const Json& body = root.at(has_result ? "result" : "error");
    if (!body.is_object())
    {
        return invalid_response(ProtocolErrorCode::invalid_field_type, request_id,
            has_result ? "result" : "error");
    }
    if (has_error)
    {
        if (!has_only_fields(body, {"code", "message"}))
        {
            return invalid_response(ProtocolErrorCode::unknown_field, request_id, "error");
        }
        if (!has_field(body, "code"))
        {
            return invalid_response(ProtocolErrorCode::missing_field, request_id, "error.code");
        }
        if (!has_field(body, "message"))
        {
            return invalid_response(ProtocolErrorCode::missing_field, request_id, "error.message");
        }
        std::string code;
        std::string message;
        if (!parse_string(body, "code", code) || !parse_string(body, "message", message))
        {
            return invalid_response(ProtocolErrorCode::invalid_field_type, request_id, "error");
        }
        if (!is_valid_code(code) || !is_valid_message(message))
        {
            return invalid_response(ProtocolErrorCode::invalid_field_value, request_id, "error");
        }
        return Response{*request_id, ErrorResponse{std::move(code), std::move(message)}};
    }

    if (!has_field(body, "type"))
    {
        return invalid_response(ProtocolErrorCode::missing_field, request_id, "result.type");
    }
    if (!body.at("type").is_string())
    {
        return invalid_response(ProtocolErrorCode::invalid_field_type, request_id, "result.type");
    }
    const std::string type = body.at("type").get<std::string>();
    if (type == "PONG" || type == "TELEMETRY_ACCEPTED")
    {
        if (!has_only_fields(body, {"type"}))
        {
            return invalid_response(ProtocolErrorCode::unknown_field, request_id, "result");
        }
        if (type == "PONG")
        {
            return Response{*request_id, Pong{}};
        }
        return Response{*request_id, TelemetryAccepted{}};
    }
    if (type == "DEVICE_REGISTERED")
    {
        if (!has_only_fields(body, {"type", "device_id"}))
        {
            return invalid_response(ProtocolErrorCode::unknown_field, request_id, "result");
        }
        if (!has_field(body, "device_id"))
        {
            return invalid_response(ProtocolErrorCode::missing_field, request_id, "result.device_id");
        }
        std::string device_id;
        if (!parse_string(body, "device_id", device_id))
        {
            return invalid_response(ProtocolErrorCode::invalid_field_type, request_id, "result.device_id");
        }
        if (!is_valid_identifier(device_id))
        {
            return invalid_response(ProtocolErrorCode::invalid_field_value, request_id, "result.device_id");
        }
        return Response{*request_id, DeviceRegistered{std::move(device_id)}};
    }
    if (type == "STATISTICS")
    {
        if (!has_only_fields(body,
                {"type", "metric", "sample_count", "minimum", "maximum", "mean", "last_value", "last_received_at"}))
        {
            return invalid_response(ProtocolErrorCode::unknown_field, request_id, "result");
        }
        for (const std::string_view field : {"metric", "sample_count", "minimum", "maximum", "mean", "last_value", "last_received_at"})
        {
            if (!has_field(body, field))
            {
                return invalid_response(ProtocolErrorCode::missing_field, request_id, "result." + std::string{field});
            }
        }
        std::string metric;
        std::string last_received_at;
        if (!parse_string(body, "metric", metric) || !parse_string(body, "last_received_at", last_received_at))
        {
            return invalid_response(ProtocolErrorCode::invalid_field_type, request_id, "result.statistics");
        }
        if (!is_valid_identifier(metric) || !is_valid_received_timestamp(last_received_at))
        {
            return invalid_response(ProtocolErrorCode::invalid_field_value, request_id, "result.statistics");
        }
        const auto count_iterator = body.find("sample_count");
        if (count_iterator == body.end())
        {
            return invalid_response(ProtocolErrorCode::missing_field, request_id, "result.sample_count");
        }
        const auto count = parse_canonical_uint64(*count_iterator);
        if (!count.has_value())
        {
            return invalid_response(ProtocolErrorCode::invalid_field_value, request_id, "result.sample_count");
        }
        Statistics statistics;
        statistics.metric = std::move(metric);
        statistics.sample_count = *count;
        statistics.last_received_at = std::move(last_received_at);
        if (!body.at("minimum").is_number() || !body.at("maximum").is_number() || !body.at("mean").is_number() ||
            !body.at("last_value").is_number())
        {
            return invalid_response(ProtocolErrorCode::invalid_field_type, request_id, "result.statistics");
        }
        if (!parse_number(body, "minimum", statistics.minimum) || !parse_number(body, "maximum", statistics.maximum) ||
            !parse_number(body, "mean", statistics.mean) || !parse_number(body, "last_value", statistics.last_value))
        {
            return invalid_response(ProtocolErrorCode::invalid_field_value, request_id, "result.statistics");
        }
        return Response{*request_id, std::move(statistics)};
    }
    return invalid_response(ProtocolErrorCode::invalid_field_value, request_id, "result.type");
}

[[nodiscard]] Json make_request_json(const Request& request, SerializationError& error)
{
    Json root = {{"version", protocol_version}, {"request_id", std::to_string(request.request_id)}};
    const bool valid = std::visit([&root, &error, id = request.request_id](const auto& command) -> bool {
        using T = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<T, Ping>)
        {
            root["command"] = "PING";
        }
        else if constexpr (std::is_same_v<T, RegisterDevice>)
        {
            if (!is_valid_utf8(command.device_id))
            {
                error = {SerializationErrorCode::invalid_utf8, id, "device_id"};
                return false;
            }
            if (!is_valid_identifier(command.device_id))
            {
                error = {SerializationErrorCode::invalid_message, id, "device_id"};
                return false;
            }
            root["command"] = "REGISTER_DEVICE";
            root["device_id"] = command.device_id;
        }
        else if constexpr (std::is_same_v<T, SubmitTelemetry>)
        {
            if (!is_valid_utf8(command.metric))
            {
                error = {SerializationErrorCode::invalid_utf8, id, "metric"};
                return false;
            }
            if (!is_valid_identifier(command.metric))
            {
                error = {SerializationErrorCode::invalid_message, id, "metric"};
                return false;
            }
            if (!is_finite(command.value))
            {
                error = {SerializationErrorCode::invalid_message, id, "value"};
                return false;
            }
            root["command"] = "SUBMIT_TELEMETRY";
            root["metric"] = command.metric;
            root["value"] = command.value;
        }
        else if constexpr (std::is_same_v<T, GetStatistics>)
        {
            if (!is_valid_utf8(command.metric))
            {
                error = {SerializationErrorCode::invalid_utf8, id, "metric"};
                return false;
            }
            if (!is_valid_identifier(command.metric))
            {
                error = {SerializationErrorCode::invalid_message, id, "metric"};
                return false;
            }
            root["command"] = "GET_STATISTICS";
            root["metric"] = command.metric;
        }
        return true;
    }, request.command);
    return valid ? root : Json{};
}

[[nodiscard]] Json make_response_json(const Response& response, SerializationError& error)
{
    Json root = {{"version", protocol_version}, {"request_id", std::to_string(response.request_id)}};
    const bool valid = std::visit([&root, &error, id = response.request_id](const auto& body) -> bool {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, Pong>)
        {
            root["result"] = {{"type", "PONG"}};
        }
        else if constexpr (std::is_same_v<T, DeviceRegistered>)
        {
            if (!is_valid_identifier(body.device_id))
            {
                error = {SerializationErrorCode::invalid_message, id, "result.device_id"};
                return false;
            }
            root["result"] = {{"type", "DEVICE_REGISTERED"}, {"device_id", body.device_id}};
        }
        else if constexpr (std::is_same_v<T, TelemetryAccepted>)
        {
            root["result"] = {{"type", "TELEMETRY_ACCEPTED"}};
        }
        else if constexpr (std::is_same_v<T, Statistics>)
        {
            if (body.last_received_at.size() > max_payload_size_bytes)
            {
                error = {SerializationErrorCode::payload_too_large, id, "result.last_received_at"};
                return false;
            }
            if (!is_valid_identifier(body.metric) || !is_valid_received_timestamp(body.last_received_at))
            {
                error = {SerializationErrorCode::invalid_message, id, "result.statistics"};
                return false;
            }
            if (!is_finite(body.minimum) || !is_finite(body.maximum) || !is_finite(body.mean) ||
                !is_finite(body.last_value))
            {
                error = {SerializationErrorCode::invalid_message, id, "result.statistics"};
                return false;
            }
            root["result"] = {{"type", "STATISTICS"},
                {"metric", body.metric},
                {"sample_count", std::to_string(body.sample_count)},
                {"minimum", body.minimum},
                {"maximum", body.maximum},
                {"mean", body.mean},
                {"last_value", body.last_value},
                {"last_received_at", body.last_received_at}};
        }
        else if constexpr (std::is_same_v<T, ErrorResponse>)
        {
            if (body.code.size() > max_payload_size_bytes || body.message.size() > max_payload_size_bytes)
            {
                error = {SerializationErrorCode::payload_too_large, id, "error"};
                return false;
            }
            if (!is_valid_code(body.code) || !is_valid_message(body.message))
            {
                error = {SerializationErrorCode::invalid_message, id, "error"};
                return false;
            }
            root["error"] = {{"code", body.code}, {"message", body.message}};
        }
        return true;
    }, response.body);
    return valid ? root : Json{};
}

[[nodiscard]] PayloadSerializationResult serialize_json(Json value,
    const SerializationError invalid_error)
{
    try
    {
        const std::string encoded = value.dump(-1, ' ', false, Json::error_handler_t::strict);
        if (encoded.size() > max_payload_size_bytes)
        {
            return {{}, SerializationError{SerializationErrorCode::payload_too_large,
                               invalid_error.request_id, {}}};
        }
        std::vector<std::byte> payload;
        payload.reserve(encoded.size());
        for (const unsigned char character : encoded)
        {
            payload.push_back(static_cast<std::byte>(character));
        }
        return {std::move(payload), std::nullopt};
    }
    catch (const Json::type_error&)
    {
        return {{}, SerializationError{SerializationErrorCode::invalid_utf8,
                           invalid_error.request_id, invalid_error.field}};
    }
}

} // namespace

RequestParseResult parse_request_payload(const std::span<const std::byte> payload)
{
    ParsedInput parsed = parse_json_payload(payload);
    if (parsed.error.has_value())
    {
        return *parsed.error;
    }
    return parse_request(parsed.value);
}

ResponseParseResult parse_response_payload(const std::span<const std::byte> payload)
{
    ParsedInput parsed = parse_json_payload(payload);
    if (parsed.error.has_value())
    {
        return *parsed.error;
    }
    return parse_response(parsed.value);
}

PayloadSerializationResult serialize_request_payload(const Request& request)
{
    SerializationError validation_error_value{SerializationErrorCode::invalid_message, request.request_id, {}};
    Json message = make_request_json(request, validation_error_value);
    if (message.is_null())
    {
        return {{}, std::move(validation_error_value)};
    }
    return serialize_json(std::move(message), std::move(validation_error_value));
}

PayloadSerializationResult serialize_response_payload(const Response& response)
{
    SerializationError validation_error_value{SerializationErrorCode::invalid_message, response.request_id, {}};
    Json message = make_response_json(response, validation_error_value);
    if (message.is_null())
    {
        return {{}, std::move(validation_error_value)};
    }
    return serialize_json(std::move(message), std::move(validation_error_value));
}

const char* to_string(const JsonInputErrorCode code) noexcept
{
    switch (code)
    {
    case JsonInputErrorCode::empty_payload: return "EMPTY_PAYLOAD";
    case JsonInputErrorCode::payload_too_large: return "PAYLOAD_TOO_LARGE";
    case JsonInputErrorCode::embedded_nul: return "EMBEDDED_NUL";
    case JsonInputErrorCode::invalid_utf8: return "INVALID_UTF8";
    case JsonInputErrorCode::malformed_json: return "MALFORMED_JSON";
    case JsonInputErrorCode::duplicate_key: return "DUPLICATE_KEY";
    case JsonInputErrorCode::nesting_too_deep: return "NESTING_TOO_DEEP";
    case JsonInputErrorCode::too_many_object_members: return "TOO_MANY_OBJECT_MEMBERS";
    }
    return "UNKNOWN_JSON_INPUT_ERROR";
}

const char* to_string(const ProtocolErrorCode code) noexcept
{
    switch (code)
    {
    case ProtocolErrorCode::unsupported_version: return "UNSUPPORTED_VERSION";
    case ProtocolErrorCode::invalid_root_type: return "INVALID_ROOT_TYPE";
    case ProtocolErrorCode::missing_field: return "MISSING_FIELD";
    case ProtocolErrorCode::unknown_field: return "UNKNOWN_FIELD";
    case ProtocolErrorCode::invalid_field_type: return "INVALID_FIELD_TYPE";
    case ProtocolErrorCode::invalid_field_value: return "INVALID_FIELD_VALUE";
    case ProtocolErrorCode::invalid_request_id: return "INVALID_REQUEST_ID";
    case ProtocolErrorCode::unknown_command: return "UNKNOWN_COMMAND";
    case ProtocolErrorCode::invalid_response_shape: return "INVALID_RESPONSE_SHAPE";
    }
    return "UNKNOWN_PROTOCOL_ERROR";
}

const char* to_string(const SerializationErrorCode code) noexcept
{
    switch (code)
    {
    case SerializationErrorCode::invalid_message: return "INVALID_MESSAGE";
    case SerializationErrorCode::invalid_utf8: return "INVALID_UTF8";
    case SerializationErrorCode::payload_too_large: return "PAYLOAD_TOO_LARGE";
    }
    return "UNKNOWN_SERIALIZATION_ERROR";
}

} // namespace telemetryhub::protocol
