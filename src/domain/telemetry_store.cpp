#include <telemetryhub/domain/telemetry_store.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace telemetryhub::domain {
namespace {

[[nodiscard]] double updated_mean(
    const double previous_mean,
    const double value,
    const std::uint64_t sample_count) noexcept
{
    const double count = static_cast<double>(sample_count);
    const double sample_weight = 1.0 / count;

    // The usual delta update can overflow when finite inputs have opposite
    // signs. This weighted form avoids that subtraction in that case.
    if (std::signbit(previous_mean) != std::signbit(value))
    {
        return previous_mean * (1.0 - sample_weight) + value * sample_weight;
    }

    return previous_mean + (value - previous_mean) * sample_weight;
}

} // namespace

TelemetryStore::TelemetryStore(const std::size_t max_entries)
    : max_entries_{max_entries}
{
    if (max_entries_ == 0U)
    {
        throw std::invalid_argument{"TelemetryStore capacity must be positive"};
    }
}

RecordSampleStatus TelemetryStore::record_sample(
    const std::string_view device_id,
    const std::string_view metric,
    const double value,
    const ReceivedAt received_at,
    const ReceiveSequence receive_sequence)
{
    if (!valid_key_component(device_id) || !valid_key_component(metric))
    {
        return RecordSampleStatus::invalid_key;
    }
    if (!std::isfinite(value))
    {
        return RecordSampleStatus::invalid_value;
    }

    // Allocate/copy the key before locking or changing store state. If an
    // allocation fails, the map remains untouched.
    Key key{std::string{device_id}, std::string{metric}};
    std::lock_guard lock{mutex_};
    const auto found = entries_.find(key);
    if (found == entries_.end())
    {
        if (entries_.size() >= max_entries_)
        {
            return RecordSampleStatus::capacity_reached;
        }

        const TelemetryStatisticsSnapshot initial{
            1U, value, value, value, value, received_at};
        entries_.emplace(std::move(key), Entry{initial, receive_sequence});
        return RecordSampleStatus::updated;
    }

    const TelemetryStatisticsSnapshot& current = found->second.statistics;
    if (current.sample_count == std::numeric_limits<std::uint64_t>::max())
    {
        return RecordSampleStatus::count_overflow;
    }

    const std::uint64_t next_count = current.sample_count + 1U;
    const double next_mean = updated_mean(current.mean, value, next_count);
    if (!std::isfinite(next_mean))
    {
        return RecordSampleStatus::numeric_failure;
    }

    TelemetryStatisticsSnapshot candidate{
        next_count,
        value < current.minimum ? value : current.minimum,
        value > current.maximum ? value : current.maximum,
        next_mean,
        current.last_value,
        current.last_received_at};
    ReceiveSequence candidate_sequence = found->second.last_receive_sequence;
    if (receive_sequence > candidate_sequence)
    {
        candidate.last_value = value;
        candidate.last_received_at = received_at;
        candidate_sequence = receive_sequence;
    }

    // These trivial assignments cannot throw; all validation and arithmetic
    // completed before the existing entry is modified.
    found->second.statistics = candidate;
    found->second.last_receive_sequence = candidate_sequence;
    return RecordSampleStatus::updated;
}

std::optional<TelemetryStatisticsSnapshot> TelemetryStore::statistics_for(
    const std::string_view device_id,
    const std::string_view metric) const
{
    if (!valid_key_component(device_id) || !valid_key_component(metric))
    {
        return std::nullopt;
    }

    const Key key{std::string{device_id}, std::string{metric}};
    std::lock_guard lock{mutex_};
    const auto found = entries_.find(key);
    if (found == entries_.end())
    {
        return std::nullopt;
    }
    return found->second.statistics;
}

bool TelemetryStore::valid_key_component(const std::string_view component) noexcept
{
    return !component.empty() &&
           component.size() <= max_statistics_key_component_size_bytes;
}

} // namespace telemetryhub::domain
