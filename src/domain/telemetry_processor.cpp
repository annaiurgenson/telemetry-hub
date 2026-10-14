#include <telemetryhub/domain/telemetry_processor.hpp>

#include <cmath>

namespace telemetryhub::domain {
namespace {

[[nodiscard]] bool valid_identifier(const std::string_view identifier) noexcept
{
    return !identifier.empty() &&
           identifier.size() <= max_statistics_key_component_size_bytes;
}

[[nodiscard]] bool valid_input(
    const std::string_view trusted_device_id,
    const std::string_view metric) noexcept
{
    return valid_identifier(trusted_device_id) && valid_identifier(metric);
}

[[nodiscard]] TelemetryProcessingError map_store_error(
    const RecordSampleStatus status) noexcept
{
    switch (status)
    {
    case RecordSampleStatus::invalid_key:
    case RecordSampleStatus::invalid_value:
        return TelemetryProcessingError::invalid_input;
    case RecordSampleStatus::count_overflow:
        return TelemetryProcessingError::count_overflow;
    case RecordSampleStatus::capacity_reached:
        return TelemetryProcessingError::capacity_reached;
    case RecordSampleStatus::numeric_failure:
        return TelemetryProcessingError::numeric_failure;
    case RecordSampleStatus::updated:
        break;
    }
    return TelemetryProcessingError::numeric_failure;
}

} // namespace

TelemetryProcessor::TelemetryProcessor(TelemetryStore& store) noexcept
    : store_{store}
{
}

TelemetrySubmissionResult TelemetryProcessor::submit_telemetry(
    const std::string_view trusted_device_id,
    const std::string_view metric,
    const double value,
    const ReceivedAt received_at,
    const ReceiveSequence receive_sequence) const
{
    if (!valid_input(trusted_device_id, metric) || !std::isfinite(value))
    {
        return TelemetryProcessingError::invalid_input;
    }

    const RecordSampleStatus status = store_.record_sample(
        trusted_device_id, metric, value, received_at, receive_sequence);
    if (status == RecordSampleStatus::updated)
    {
        return TelemetryAcceptedResult{};
    }
    return map_store_error(status);
}

StatisticsQueryExecutionResult TelemetryProcessor::get_statistics(
    const std::string_view trusted_device_id,
    const std::string_view metric) const
{
    if (!valid_input(trusted_device_id, metric))
    {
        return TelemetryProcessingError::invalid_input;
    }

    const auto statistics = store_.statistics_for(trusted_device_id, metric);
    if (!statistics.has_value())
    {
        return TelemetryProcessingError::statistics_not_found;
    }

    return StatisticsQueryResult{std::string{metric}, *statistics};
}

} // namespace telemetryhub::domain
