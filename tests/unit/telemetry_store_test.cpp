#include <telemetryhub/domain/telemetry_store.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <limits>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace telemetryhub::domain {
namespace {

[[nodiscard]] ReceivedAt received_at(const std::int64_t milliseconds)
{
    return ReceivedAt{std::chrono::milliseconds{milliseconds}};
}

TEST(TelemetryStoreTest, FirstSampleCreatesCompleteStatistics)
{
    TelemetryStore store;
    const ReceivedAt timestamp = received_at(1234);

    EXPECT_EQ(store.record_sample("device-a", "temperature", 21.5, timestamp, 7U),
              RecordSampleStatus::updated);

    const auto result = store.statistics_for("device-a", "temperature");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sample_count, 1U);
    EXPECT_DOUBLE_EQ(result->minimum, 21.5);
    EXPECT_DOUBLE_EQ(result->maximum, 21.5);
    EXPECT_DOUBLE_EQ(result->mean, 21.5);
    EXPECT_DOUBLE_EQ(result->last_value, 21.5);
    EXPECT_EQ(result->last_received_at, timestamp);
}

TEST(TelemetryStoreTest, RepeatedSamplesCalculateAggregateStatistics)
{
    TelemetryStore store;
    const double values[]{-10.0, 0.0, 2.0, 8.0};
    for (std::size_t index = 0; index < std::size(values); ++index)
    {
        EXPECT_EQ(store.record_sample(
                      "device-a", "temperature", values[index],
                      received_at(static_cast<std::int64_t>(index)), index + 1U),
                  RecordSampleStatus::updated);
    }

    const auto result = store.statistics_for("device-a", "temperature");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sample_count, 4U);
    EXPECT_DOUBLE_EQ(result->minimum, -10.0);
    EXPECT_DOUBLE_EQ(result->maximum, 8.0);
    EXPECT_NEAR(result->mean, 0.0, 1e-12);
    EXPECT_DOUBLE_EQ(result->last_value, 8.0);
}

TEST(TelemetryStoreTest, KeepsDevicesAndMetricsIndependent)
{
    TelemetryStore store;
    EXPECT_EQ(store.record_sample("device-a", "temperature", 20.0, received_at(1), 1U),
              RecordSampleStatus::updated);
    EXPECT_EQ(store.record_sample("device-a", "humidity", 40.0, received_at(2), 2U),
              RecordSampleStatus::updated);
    EXPECT_EQ(store.record_sample("device-b", "temperature", -3.0, received_at(3), 3U),
              RecordSampleStatus::updated);

    ASSERT_TRUE(store.statistics_for("device-a", "temperature").has_value());
    EXPECT_DOUBLE_EQ(store.statistics_for("device-a", "temperature")->mean, 20.0);
    EXPECT_DOUBLE_EQ(store.statistics_for("device-a", "humidity")->mean, 40.0);
    EXPECT_DOUBLE_EQ(store.statistics_for("device-b", "temperature")->mean, -3.0);
}

TEST(TelemetryStoreTest, MeanHandlesFractionalAndExtremeFiniteInputs)
{
    TelemetryStore store;
    EXPECT_EQ(store.record_sample("d", "m", DBL_MAX, received_at(1), 1U),
              RecordSampleStatus::updated);
    EXPECT_EQ(store.record_sample("d", "m", -DBL_MAX, received_at(2), 2U),
              RecordSampleStatus::updated);

    auto result = store.statistics_for("d", "m");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sample_count, 2U);
    EXPECT_DOUBLE_EQ(result->minimum, -DBL_MAX);
    EXPECT_DOUBLE_EQ(result->maximum, DBL_MAX);
    EXPECT_TRUE(std::isfinite(result->mean));
    EXPECT_NEAR(result->mean, 0.0, 1e-12);

    EXPECT_EQ(store.record_sample("d", "m", DBL_MAX, received_at(3), 3U),
              RecordSampleStatus::updated);
    result = store.statistics_for("d", "m");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(std::isfinite(result->mean));
    EXPECT_NEAR(result->mean / DBL_MAX, 1.0 / 3.0, 1e-15);

    TelemetryStore fractional;
    EXPECT_EQ(fractional.record_sample("d", "m", -1.25, received_at(1), 1U),
              RecordSampleStatus::updated);
    EXPECT_EQ(fractional.record_sample("d", "m", 0.0, received_at(2), 2U),
              RecordSampleStatus::updated);
    EXPECT_EQ(fractional.record_sample("d", "m", 2.5, received_at(3), 3U),
              RecordSampleStatus::updated);
    EXPECT_NEAR(fractional.statistics_for("d", "m")->mean, 0.4166666666666667, 1e-15);
}

TEST(TelemetryStoreTest, RejectsNonFiniteValuesWithoutChangingStatistics)
{
    TelemetryStore store;
    ASSERT_EQ(store.record_sample("d", "m", 3.0, received_at(10), 10U),
              RecordSampleStatus::updated);
    const auto before = store.statistics_for("d", "m");

    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity()})
    {
        EXPECT_EQ(store.record_sample("d", "m", invalid, received_at(20), 20U),
                  RecordSampleStatus::invalid_value);
    }
    EXPECT_EQ(store.statistics_for("d", "m"), before);
}

TEST(TelemetryStoreTest, RejectsInvalidAndOversizedKeyComponents)
{
    TelemetryStore store;
    const std::string valid_component(max_statistics_key_component_size_bytes, 'x');
    const std::string oversized_component(max_statistics_key_component_size_bytes + 1U, 'x');

    EXPECT_EQ(store.record_sample("", "m", 1.0, received_at(0), 1U),
              RecordSampleStatus::invalid_key);
    EXPECT_EQ(store.record_sample("d", "", 1.0, received_at(0), 1U),
              RecordSampleStatus::invalid_key);
    EXPECT_EQ(store.record_sample(oversized_component, "m", 1.0, received_at(0), 1U),
              RecordSampleStatus::invalid_key);
    EXPECT_EQ(store.record_sample("d", oversized_component, 1.0, received_at(0), 1U),
              RecordSampleStatus::invalid_key);
    EXPECT_EQ(store.record_sample(valid_component, "m", 1.0, received_at(0), 1U),
              RecordSampleStatus::updated);
    EXPECT_EQ(store.statistics_for("", "m"), std::nullopt);
    EXPECT_EQ(store.statistics_for(oversized_component, "m"), std::nullopt);
}

TEST(TelemetryStoreTest, ReturnsNoValueForUnknownKeys)
{
    TelemetryStore store;
    EXPECT_EQ(store.statistics_for("unknown-device", "temperature"), std::nullopt);
}

TEST(TelemetryStoreTest, LastFieldsFollowGreatestReceiveSequence)
{
    TelemetryStore store;
    const ReceivedAt later_time = received_at(500);
    const ReceivedAt earlier_time = received_at(100);

    ASSERT_EQ(store.record_sample("d", "m", 10.0, later_time, 20U),
              RecordSampleStatus::updated);
    ASSERT_EQ(store.record_sample("d", "m", 2.0, earlier_time, 10U),
              RecordSampleStatus::updated);

    const auto result = store.statistics_for("d", "m");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sample_count, 2U);
    EXPECT_DOUBLE_EQ(result->minimum, 2.0);
    EXPECT_DOUBLE_EQ(result->maximum, 10.0);
    EXPECT_DOUBLE_EQ(result->mean, 6.0);
    EXPECT_DOUBLE_EQ(result->last_value, 10.0);
    EXPECT_EQ(result->last_received_at, later_time);
}

TEST(TelemetryStoreTest, EqualSequenceKeepsPreviouslyStoredLastFields)
{
    TelemetryStore store;
    const ReceivedAt first_time = received_at(100);
    const ReceivedAt tied_time = received_at(200);

    ASSERT_EQ(store.record_sample("d", "m", 1.0, first_time, 5U),
              RecordSampleStatus::updated);
    ASSERT_EQ(store.record_sample("d", "m", 3.0, tied_time, 5U),
              RecordSampleStatus::updated);

    const auto result = store.statistics_for("d", "m");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sample_count, 2U);
    EXPECT_DOUBLE_EQ(result->mean, 2.0);
    EXPECT_DOUBLE_EQ(result->last_value, 1.0);
    EXPECT_EQ(result->last_received_at, first_time);
}

TEST(TelemetryStoreTest, CapacityRejectsNewKeysButAllowsExistingKeyUpdates)
{
    TelemetryStore store{2U};
    ASSERT_EQ(store.record_sample("d1", "m", 1.0, received_at(1), 1U),
              RecordSampleStatus::updated);
    ASSERT_EQ(store.record_sample("d2", "m", 2.0, received_at(2), 2U),
              RecordSampleStatus::updated);

    EXPECT_EQ(store.record_sample("d3", "m", 3.0, received_at(3), 3U),
              RecordSampleStatus::capacity_reached);
    EXPECT_EQ(store.record_sample("d1", "m", 5.0, received_at(4), 4U),
              RecordSampleStatus::updated);
    EXPECT_EQ(store.statistics_for("d3", "m"), std::nullopt);
    EXPECT_EQ(store.statistics_for("d1", "m")->sample_count, 2U);
}

TEST(TelemetryStoreTest, CapacityMustBePositive)
{
    EXPECT_THROW(TelemetryStore{0U}, std::invalid_argument);
}

TEST(TelemetryStoreTest, FailedStatusUpdatesLeavePriorStatisticsUnchanged)
{
    TelemetryStore store{1U};
    ASSERT_EQ(store.record_sample("d", "m", 4.0, received_at(1), 1U),
              RecordSampleStatus::updated);
    const auto before = store.statistics_for("d", "m");

    EXPECT_EQ(store.record_sample("d", "m", std::numeric_limits<double>::infinity(),
                                  received_at(2), 2U),
              RecordSampleStatus::invalid_value);
    EXPECT_EQ(store.record_sample("another", "m", 9.0, received_at(3), 3U),
              RecordSampleStatus::capacity_reached);
    EXPECT_EQ(store.record_sample("", "m", 9.0, received_at(4), 4U),
              RecordSampleStatus::invalid_key);
    EXPECT_EQ(store.statistics_for("d", "m"), before);
}

TEST(TelemetryStoreTest, ConcurrentUpdatesToSameKeyRemainConsistent)
{
    constexpr std::size_t worker_count = 6U;
    constexpr std::size_t samples_per_worker = 1'000U;
    TelemetryStore store;
    std::latch start{static_cast<std::ptrdiff_t>(worker_count + 1U)};
    std::atomic<bool> unexpected_status{false};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker = 0; worker < worker_count; ++worker)
    {
        workers.emplace_back([&, worker] {
            start.count_down();
            start.wait();
            const double value = static_cast<double>(worker + 1U);
            for (std::size_t index = 0; index < samples_per_worker; ++index)
            {
                const std::uint64_t sequence =
                    static_cast<std::uint64_t>(worker * samples_per_worker + index + 1U);
                if (store.record_sample("shared-device", "m", value,
                                        received_at(static_cast<std::int64_t>(sequence)), sequence) !=
                    RecordSampleStatus::updated)
                {
                    unexpected_status.store(true);
                }
            }
        });
    }
    start.count_down();
    start.wait();
    for (auto& worker : workers)
    {
        worker.join();
    }

    EXPECT_FALSE(unexpected_status.load());
    const auto result = store.statistics_for("shared-device", "m");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sample_count, worker_count * samples_per_worker);
    EXPECT_DOUBLE_EQ(result->minimum, 1.0);
    EXPECT_DOUBLE_EQ(result->maximum, static_cast<double>(worker_count));
    EXPECT_NEAR(result->mean, (static_cast<double>(worker_count) + 1.0) / 2.0, 1e-12);
    EXPECT_DOUBLE_EQ(result->last_value, static_cast<double>(worker_count));
    EXPECT_EQ(result->last_received_at,
              received_at(static_cast<std::int64_t>(worker_count * samples_per_worker)));
}

TEST(TelemetryStoreTest, ConcurrentUpdatesToDifferentKeysRemainIndependent)
{
    constexpr std::size_t worker_count = 8U;
    constexpr std::size_t samples_per_worker = 500U;
    TelemetryStore store{worker_count};
    std::latch start{static_cast<std::ptrdiff_t>(worker_count + 1U)};
    std::atomic<bool> unexpected_status{false};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker = 0; worker < worker_count; ++worker)
    {
        workers.emplace_back([&, worker] {
            const std::string device = "device-" + std::to_string(worker);
            const double value = -static_cast<double>(worker);
            start.count_down();
            start.wait();
            for (std::size_t index = 0; index < samples_per_worker; ++index)
            {
                if (store.record_sample(device, "m", value,
                                        received_at(static_cast<std::int64_t>(index)),
                                        static_cast<std::uint64_t>(index + 1U)) !=
                    RecordSampleStatus::updated)
                {
                    unexpected_status.store(true);
                }
            }
        });
    }
    start.count_down();
    start.wait();
    for (auto& worker : workers)
    {
        worker.join();
    }

    EXPECT_FALSE(unexpected_status.load());
    for (std::size_t worker = 0; worker < worker_count; ++worker)
    {
        const auto result = store.statistics_for("device-" + std::to_string(worker), "m");
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->sample_count, samples_per_worker);
        EXPECT_DOUBLE_EQ(result->minimum, -static_cast<double>(worker));
        EXPECT_DOUBLE_EQ(result->maximum, -static_cast<double>(worker));
        EXPECT_DOUBLE_EQ(result->mean, -static_cast<double>(worker));
    }
}

TEST(TelemetryStoreTest, QueriesConcurrentWithUpdatesReturnConsistentSnapshots)
{
    constexpr std::size_t update_count = 20'000U;
    TelemetryStore store;
    ASSERT_EQ(store.record_sample("d", "m", 0.0, received_at(0), 0U),
              RecordSampleStatus::updated);

    std::latch start{2};
    std::atomic<bool> writer_done{false};
    std::atomic<bool> inconsistent_snapshot{false};
    std::atomic<std::size_t> snapshots_read{0U};

    std::thread writer{[&] {
        start.count_down();
        start.wait();
        for (std::size_t index = 1; index <= update_count; ++index)
        {
            const double value = static_cast<double>(index % 101U) - 50.0;
            const auto sequence = static_cast<std::uint64_t>(index);
            if (store.record_sample("d", "m", value,
                                    received_at(static_cast<std::int64_t>(index)), sequence) !=
                RecordSampleStatus::updated)
            {
                inconsistent_snapshot.store(true);
            }
        }
        writer_done.store(true);
    }};
    std::thread reader{[&] {
        start.count_down();
        start.wait();
        do
        {
            const auto result = store.statistics_for("d", "m");
            if (!result.has_value() || result->sample_count == 0U ||
                result->minimum > result->mean || result->mean > result->maximum ||
                result->minimum > result->maximum || result->last_value < result->minimum ||
                result->last_value > result->maximum)
            {
                inconsistent_snapshot.store(true);
            }
            snapshots_read.fetch_add(1U);
        } while (!writer_done.load());
    }};
    start.wait();
    writer.join();
    reader.join();

    EXPECT_FALSE(inconsistent_snapshot.load());
    EXPECT_GT(snapshots_read.load(), 0U);
    EXPECT_EQ(store.statistics_for("d", "m")->sample_count, update_count + 1U);
}

} // namespace
} // namespace telemetryhub::domain
