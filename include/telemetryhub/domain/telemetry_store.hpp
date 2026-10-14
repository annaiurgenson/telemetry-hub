#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace telemetryhub::domain {

inline constexpr std::size_t default_max_statistics_entries = 65'536U;
inline constexpr std::size_t max_statistics_key_component_size_bytes = 128U;

using ReceivedAt = std::chrono::system_clock::time_point;
using ReceiveSequence = std::uint64_t;

struct TelemetryStatisticsSnapshot
{
    std::uint64_t sample_count{};
    double minimum{};
    double maximum{};
    double mean{};
    double last_value{};
    ReceivedAt last_received_at{};
    bool operator==(const TelemetryStatisticsSnapshot&) const = default;
};

enum class RecordSampleStatus
{
    updated,
    invalid_key,
    invalid_value,
    count_overflow,
    capacity_reached,
    numeric_failure
};

class TelemetryStore
{
public:
    explicit TelemetryStore(
        std::size_t max_entries = default_max_statistics_entries);

    [[nodiscard]] RecordSampleStatus record_sample(
        std::string_view device_id,
        std::string_view metric,
        double value,
        ReceivedAt received_at,
        ReceiveSequence receive_sequence);

    [[nodiscard]] std::optional<TelemetryStatisticsSnapshot> statistics_for(
        std::string_view device_id,
        std::string_view metric) const;

private:
    using Key = std::pair<std::string, std::string>;

    struct Entry
    {
        TelemetryStatisticsSnapshot statistics;
        ReceiveSequence last_receive_sequence{};
    };

    [[nodiscard]] static bool valid_key_component(std::string_view component) noexcept;

    const std::size_t max_entries_;
    mutable std::mutex mutex_;
    std::map<Key, Entry> entries_;
};

} // namespace telemetryhub::domain
