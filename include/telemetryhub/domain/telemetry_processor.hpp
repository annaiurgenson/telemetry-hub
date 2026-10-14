#pragma once

#include <telemetryhub/domain/telemetry_store.hpp>

#include <string>
#include <string_view>
#include <variant>

namespace telemetryhub::domain {

struct TelemetryAcceptedResult
{
    bool operator==(const TelemetryAcceptedResult&) const = default;
};

struct StatisticsQueryResult
{
    std::string metric;
    TelemetryStatisticsSnapshot statistics;
};

enum class TelemetryProcessingError
{
    invalid_input,
    statistics_not_found,
    capacity_reached,
    count_overflow,
    numeric_failure
};

using TelemetrySubmissionResult =
    std::variant<TelemetryAcceptedResult, TelemetryProcessingError>;
using StatisticsQueryExecutionResult =
    std::variant<StatisticsQueryResult, TelemetryProcessingError>;

class TelemetryProcessor
{
public:
    explicit TelemetryProcessor(TelemetryStore& store) noexcept;

    [[nodiscard]] TelemetrySubmissionResult submit_telemetry(
        std::string_view trusted_device_id,
        std::string_view metric,
        double value,
        ReceivedAt received_at,
        ReceiveSequence receive_sequence) const;

    [[nodiscard]] StatisticsQueryExecutionResult get_statistics(
        std::string_view trusted_device_id,
        std::string_view metric) const;

private:
    TelemetryStore& store_;
};

} // namespace telemetryhub::domain
