#include <telemetryhub/domain/telemetry_processor.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <limits>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace telemetryhub::domain {
namespace {

[[nodiscard]] ReceivedAt received_at(const std::int64_t milliseconds)
{
    return ReceivedAt{std::chrono::milliseconds{milliseconds}};
}

[[nodiscard]] TelemetryProcessingError error_of(
    const TelemetrySubmissionResult& result)
{
    return std::get<TelemetryProcessingError>(result);
}

[[nodiscard]] TelemetryProcessingError error_of(
    const StatisticsQueryExecutionResult& result)
{
    return std::get<TelemetryProcessingError>(result);
}

TEST(TelemetryProcessorTest, SuccessfulSubmissionReturnsAcceptedAlternative)
{
    TelemetryStore store;
    const TelemetryProcessor processor{store};

    const auto result = processor.submit_telemetry(
        "device-a", "temperature", 21.5, received_at(1234), 7U);

    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(result));
    EXPECT_EQ(store.statistics_for("device-a", "temperature")->sample_count, 1U);
}

TEST(TelemetryProcessorTest, StatisticsIncludeAllAggregateFields)
{
    TelemetryStore store;
    const TelemetryProcessor processor{store};
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("device-a", "temperature", 2.0, received_at(10), 1U)));
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("device-a", "temperature", 8.0, received_at(20), 2U)));

    const auto result = processor.get_statistics("device-a", "temperature");
    ASSERT_TRUE(std::holds_alternative<StatisticsQueryResult>(result));
    const auto& query = std::get<StatisticsQueryResult>(result);
    EXPECT_EQ(query.metric, "temperature");
    EXPECT_EQ(query.statistics.sample_count, 2U);
    EXPECT_DOUBLE_EQ(query.statistics.minimum, 2.0);
    EXPECT_DOUBLE_EQ(query.statistics.maximum, 8.0);
    EXPECT_DOUBLE_EQ(query.statistics.mean, 5.0);
    EXPECT_DOUBLE_EQ(query.statistics.last_value, 8.0);
    EXPECT_EQ(query.statistics.last_received_at, received_at(20));
}

TEST(TelemetryProcessorTest, KeepsDevicesAndMetricsIndependent)
{
    TelemetryStore store;
    const TelemetryProcessor processor{store};
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("a", "temperature", 10.0, received_at(1), 1U)));
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("a", "humidity", 20.0, received_at(2), 2U)));
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("b", "temperature", 30.0, received_at(3), 3U)));

    EXPECT_DOUBLE_EQ(std::get<StatisticsQueryResult>(
                         processor.get_statistics("a", "temperature")).statistics.mean,
                     10.0);
    EXPECT_DOUBLE_EQ(std::get<StatisticsQueryResult>(
                         processor.get_statistics("a", "humidity")).statistics.mean,
                     20.0);
    EXPECT_DOUBLE_EQ(std::get<StatisticsQueryResult>(
                         processor.get_statistics("b", "temperature")).statistics.mean,
                     30.0);
}

TEST(TelemetryProcessorTest, ValidUnknownMetricReturnsStatisticsNotFound)
{
    TelemetryStore store;
    const TelemetryProcessor processor{store};

    const auto result = processor.get_statistics("device-a", "unknown");

    ASSERT_TRUE(std::holds_alternative<TelemetryProcessingError>(result));
    EXPECT_EQ(error_of(result), TelemetryProcessingError::statistics_not_found);
}

TEST(TelemetryProcessorTest, InvalidIdentifiersReturnInvalidInputForBothOperations)
{
    TelemetryStore store;
    const TelemetryProcessor processor{store};
    const std::string valid(max_statistics_key_component_size_bytes, 'x');
    const std::string oversized(max_statistics_key_component_size_bytes + 1U, 'x');

    for (const auto& [device, metric] : std::vector<std::pair<std::string, std::string>>{
             {"", "m"}, {"d", ""}, {oversized, "m"}, {"d", oversized}})
    {
        EXPECT_EQ(error_of(processor.submit_telemetry(
                      device, metric, 1.0, received_at(1), 1U)),
                  TelemetryProcessingError::invalid_input);
        EXPECT_EQ(error_of(processor.get_statistics(device, metric)),
                  TelemetryProcessingError::invalid_input);
    }

    EXPECT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry(valid, valid, 1.0, received_at(1), 1U)));
}

TEST(TelemetryProcessorTest, NonFiniteValuesReturnInvalidInput)
{
    TelemetryStore store;
    const TelemetryProcessor processor{store};

    for (const double value : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()})
    {
        const auto result = processor.submit_telemetry(
            "d", "m", value, received_at(1), 1U);
        ASSERT_TRUE(std::holds_alternative<TelemetryProcessingError>(result));
        EXPECT_EQ(error_of(result), TelemetryProcessingError::invalid_input);
    }
    EXPECT_FALSE(store.statistics_for("d", "m").has_value());
}

TEST(TelemetryProcessorTest, CapacityFailureMapsToCapacityReached)
{
    TelemetryStore store{1U};
    const TelemetryProcessor processor{store};
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("d1", "m", 1.0, received_at(1), 1U)));

    const auto result = processor.submit_telemetry("d2", "m", 2.0, received_at(2), 2U);

    ASSERT_TRUE(std::holds_alternative<TelemetryProcessingError>(result));
    EXPECT_EQ(error_of(result), TelemetryProcessingError::capacity_reached);
}

TEST(TelemetryProcessorTest, ExistingKeyRemainsUpdateableAtCapacity)
{
    TelemetryStore store{1U};
    const TelemetryProcessor processor{store};
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("d", "m", 1.0, received_at(1), 1U)));

    const auto result = processor.submit_telemetry("d", "m", 3.0, received_at(2), 2U);

    EXPECT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(result));
    const auto query = processor.get_statistics("d", "m");
    ASSERT_TRUE(std::holds_alternative<StatisticsQueryResult>(query));
    EXPECT_EQ(std::get<StatisticsQueryResult>(query).statistics.sample_count, 2U);
}

TEST(TelemetryProcessorTest, GreatestReceiveSequenceControlsLastValueAndTimestamp)
{
    TelemetryStore store;
    const TelemetryProcessor processor{store};
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("d", "m", 10.0, received_at(500), 20U)));
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("d", "m", 2.0, received_at(100), 10U)));

    const auto result = processor.get_statistics("d", "m");
    ASSERT_TRUE(std::holds_alternative<StatisticsQueryResult>(result));
    const auto& stats = std::get<StatisticsQueryResult>(result).statistics;
    EXPECT_EQ(stats.sample_count, 2U);
    EXPECT_DOUBLE_EQ(stats.minimum, 2.0);
    EXPECT_DOUBLE_EQ(stats.maximum, 10.0);
    EXPECT_DOUBLE_EQ(stats.mean, 6.0);
    EXPECT_DOUBLE_EQ(stats.last_value, 10.0);
    EXPECT_EQ(stats.last_received_at, received_at(500));
}

TEST(TelemetryProcessorTest, EqualReceiveSequenceRetainsEarlierLastFields)
{
    TelemetryStore store;
    const TelemetryProcessor processor{store};
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("d", "m", 1.0, received_at(100), 5U)));
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("d", "m", 3.0, received_at(200), 5U)));

    const auto result = processor.get_statistics("d", "m");
    ASSERT_TRUE(std::holds_alternative<StatisticsQueryResult>(result));
    const auto& stats = std::get<StatisticsQueryResult>(result).statistics;
    EXPECT_EQ(stats.sample_count, 2U);
    EXPECT_DOUBLE_EQ(stats.mean, 2.0);
    EXPECT_DOUBLE_EQ(stats.last_value, 1.0);
    EXPECT_EQ(stats.last_received_at, received_at(100));
}

TEST(TelemetryProcessorTest, QueryResultOwnsMetricAndSnapshotAfterLaterUpdates)
{
    TelemetryStore store;
    const TelemetryProcessor processor{store};
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("d", "metric-name", 4.0, received_at(1), 1U)));
    std::string metric = "metric-name";
    const auto result = processor.get_statistics("d", metric);
    ASSERT_TRUE(std::holds_alternative<StatisticsQueryResult>(result));
    metric.assign("overwritten");
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("d", "metric-name", 8.0, received_at(2), 2U)));

    const auto& owned = std::get<StatisticsQueryResult>(result);
    EXPECT_EQ(owned.metric, "metric-name");
    EXPECT_EQ(owned.statistics.sample_count, 1U);
    EXPECT_DOUBLE_EQ(owned.statistics.mean, 4.0);
}

TEST(TelemetryProcessorTest, SuccessIsReturnedOnlyAfterUpdateIsVisible)
{
    TelemetryStore store;
    const TelemetryProcessor processor{store};

    const auto submitted = processor.submit_telemetry(
        "d", "m", 9.0, received_at(9), 9U);
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(submitted));
    const auto query = processor.get_statistics("d", "m");
    ASSERT_TRUE(std::holds_alternative<StatisticsQueryResult>(query));
    EXPECT_DOUBLE_EQ(std::get<StatisticsQueryResult>(query).statistics.last_value, 9.0);
}

TEST(TelemetryProcessorTest, ConcurrentSubmissionsThroughOneProcessorRemainConsistent)
{
    constexpr std::size_t thread_count = 6U;
    constexpr std::size_t submissions_per_thread = 500U;
    TelemetryStore store;
    const TelemetryProcessor processor{store};
    std::latch start{static_cast<std::ptrdiff_t>(thread_count + 1U)};
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (std::size_t worker = 0; worker < thread_count; ++worker)
    {
        workers.emplace_back([&, worker] {
            start.count_down();
            start.wait();
            for (std::size_t index = 0; index < submissions_per_thread; ++index)
            {
                const auto sequence = static_cast<std::uint64_t>(
                    worker * submissions_per_thread + index + 1U);
                const auto result = processor.submit_telemetry(
                    "shared", "m", static_cast<double>(worker),
                    received_at(static_cast<std::int64_t>(sequence)), sequence);
                if (!std::holds_alternative<TelemetryAcceptedResult>(result))
                {
                    failed.store(true);
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

    EXPECT_FALSE(failed.load());
    const auto result = processor.get_statistics("shared", "m");
    ASSERT_TRUE(std::holds_alternative<StatisticsQueryResult>(result));
    const auto& stats = std::get<StatisticsQueryResult>(result).statistics;
    EXPECT_EQ(stats.sample_count, thread_count * submissions_per_thread);
    EXPECT_DOUBLE_EQ(stats.minimum, 0.0);
    EXPECT_DOUBLE_EQ(stats.maximum, static_cast<double>(thread_count - 1U));
    EXPECT_GE(stats.mean, stats.minimum);
    EXPECT_LE(stats.mean, stats.maximum);
    EXPECT_DOUBLE_EQ(stats.last_value, static_cast<double>(thread_count - 1U));
    EXPECT_EQ(stats.last_received_at,
              received_at(static_cast<std::int64_t>(thread_count * submissions_per_thread)));
}

TEST(TelemetryProcessorTest, ConcurrentQueriesAndSubmissionsReturnConsistentSnapshots)
{
    constexpr std::size_t submission_count = 10'000U;
    TelemetryStore store;
    const TelemetryProcessor processor{store};
    ASSERT_TRUE(std::holds_alternative<TelemetryAcceptedResult>(
        processor.submit_telemetry("d", "m", 0.0, received_at(0), 0U)));

    std::latch start{3};
    std::atomic<bool> writer_done{false};
    std::atomic<bool> failed{false};
    std::atomic<std::size_t> snapshots_read{0U};
    std::thread writer{[&] {
        start.count_down();
        start.wait();
        for (std::size_t index = 1; index <= submission_count; ++index)
        {
            const double value = static_cast<double>(index % 101U) - 50.0;
            const auto sequence = static_cast<std::uint64_t>(index);
            if (!std::holds_alternative<TelemetryAcceptedResult>(processor.submit_telemetry(
                    "d", "m", value, received_at(static_cast<std::int64_t>(index)), sequence)))
            {
                failed.store(true);
            }
        }
        writer_done.store(true);
    }};
    std::thread reader{[&] {
        start.count_down();
        start.wait();
        do
        {
            const auto result = processor.get_statistics("d", "m");
            if (!std::holds_alternative<StatisticsQueryResult>(result))
            {
                failed.store(true);
                continue;
            }
            const auto& stats = std::get<StatisticsQueryResult>(result).statistics;
            if (stats.sample_count == 0U || stats.minimum > stats.mean ||
                stats.mean > stats.maximum || stats.minimum > stats.maximum ||
                stats.last_value < stats.minimum || stats.last_value > stats.maximum)
            {
                failed.store(true);
            }
            snapshots_read.fetch_add(1U);
        } while (!writer_done.load());
    }};
    start.count_down();
    writer.join();
    reader.join();

    EXPECT_FALSE(failed.load());
    EXPECT_GT(snapshots_read.load(), 0U);
    const auto final = processor.get_statistics("d", "m");
    ASSERT_TRUE(std::holds_alternative<StatisticsQueryResult>(final));
    EXPECT_EQ(std::get<StatisticsQueryResult>(final).statistics.sample_count,
              submission_count + 1U);
}

} // namespace
} // namespace telemetryhub::domain
