#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

namespace telemetryhub::protocol {

inline constexpr std::uint32_t protocol_version = 1U;
inline constexpr std::size_t max_identifier_size_bytes = 128U;
inline constexpr std::size_t max_json_nesting_depth = 16U;
inline constexpr std::size_t max_json_object_members = 16U;

using RequestId = std::uint64_t;

struct Ping
{
    bool operator==(const Ping&) const = default;
};

struct RegisterDevice
{
    std::string device_id;
    bool operator==(const RegisterDevice&) const = default;
};

struct SubmitTelemetry
{
    std::string metric;
    double value{};
    bool operator==(const SubmitTelemetry&) const = default;
};

struct GetStatistics
{
    std::string metric;
    bool operator==(const GetStatistics&) const = default;
};

using Command = std::variant<Ping, RegisterDevice, SubmitTelemetry, GetStatistics>;

struct Request
{
    RequestId request_id{};
    Command command;
    bool operator==(const Request&) const = default;
};

struct Pong
{
    bool operator==(const Pong&) const = default;
};

struct DeviceRegistered
{
    std::string device_id;
    bool operator==(const DeviceRegistered&) const = default;
};

struct TelemetryAccepted
{
    bool operator==(const TelemetryAccepted&) const = default;
};

struct Statistics
{
    std::string metric;
    std::uint64_t sample_count{};
    double minimum{};
    double maximum{};
    double mean{};
    double last_value{};
    std::string last_received_at;
    bool operator==(const Statistics&) const = default;
};

struct ErrorResponse
{
    std::string code;
    std::string message;
    bool operator==(const ErrorResponse&) const = default;
};

using ResponseBody = std::variant<Pong, DeviceRegistered, TelemetryAccepted, Statistics, ErrorResponse>;

struct Response
{
    RequestId request_id{};
    ResponseBody body;
    bool operator==(const Response&) const = default;
};

} // namespace telemetryhub::protocol
