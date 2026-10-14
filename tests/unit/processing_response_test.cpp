#include <telemetryhub/domain/telemetry_processor.hpp>
#include <telemetryhub/domain/telemetry_store.hpp>
#include <telemetryhub/protocol/messages.hpp>
#include <telemetryhub/server/processing_response.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <variant>

namespace telemetryhub::server {
namespace {

using namespace std::chrono;
using domain::ReceiveSequence;
using domain::ReceivedAt;
using network::ConnectionId;
using protocol::ErrorResponse;
using protocol::Response;
using server::CommandEnvelope;
using server::ProcessingCommand;

[[nodiscard]] CommandEnvelope submit_envelope(
    const std::string& device,
    const std::string& metric,
    const double value,
    const ReceivedAt received_at = ReceivedAt{},
    const ReceiveSequence sequence = 1U)
{
    return CommandEnvelope{
        ConnectionId{41U}, 72U, device,
        ProcessingCommand{protocol::SubmitTelemetry{metric, value}},
        received_at, sequence};
}

[[nodiscard]] CommandEnvelope query_envelope(
    const std::string& device,
    const std::string& metric)
{
    return CommandEnvelope{
        ConnectionId{41U}, 73U, device,
        ProcessingCommand{protocol::GetStatistics{metric}}, ReceivedAt{}, 2U};
}

[[nodiscard]] const ErrorResponse& error_of(const Response& response)
{
    return std::get<ErrorResponse>(response.body);
}

TEST(ProcessingResponse, FormatsEpochAndTruncatesToMilliseconds)
{
    const ReceivedAt timestamp{microseconds{123'456}};

    const auto formatted = format_received_timestamp(timestamp);

    ASSERT_TRUE(formatted.has_value());
    EXPECT_EQ(*formatted, "1970-01-01T00:00:00.123Z");
}

TEST(ProcessingResponse, FormatsNegativeTimestampUsingUtcCalendar)
{
    const ReceivedAt timestamp{milliseconds{-1}};

    const auto formatted = format_received_timestamp(timestamp);

    ASSERT_TRUE(formatted.has_value());
    EXPECT_EQ(*formatted, "1969-12-31T23:59:59.999Z");
}

TEST(ProcessingResponse, FormatsLeapDayAndFourDigitYear)
{
    const year_month_day date{year{2024}, month{2}, day{29}};
    const auto timestamp = ReceivedAt{sys_days{date}} + hours{3} + minutes{4} + seconds{5};

    const auto formatted = format_received_timestamp(timestamp);

    ASSERT_TRUE(formatted.has_value());
    EXPECT_EQ(*formatted, "2024-02-29T03:04:05.000Z");
}

TEST(ProcessingResponse, RejectsTimestampOutsideFourDigitYearRange)
{
    const ReceivedAt timestamp{days{3'000'000}};

    EXPECT_FALSE(format_received_timestamp(timestamp).has_value());
}

TEST(ProcessingResponse, MapsSubmissionAndStatisticsWithAllFields)
{
    domain::TelemetryStore store;
    domain::TelemetryProcessor processor{store};
    const auto received_at = ReceivedAt{milliseconds{123}};

    auto submitted = make_processing_response(
        submit_envelope("device", "temperature", 2.0, received_at, 1U), processor);
    ASSERT_EQ(submitted.request_id, 72U);
    EXPECT_TRUE(std::holds_alternative<protocol::TelemetryAccepted>(submitted.body));

    auto submitted_again = make_processing_response(
        submit_envelope("device", "temperature", 8.0, ReceivedAt{milliseconds{456}}, 2U), processor);
    ASSERT_EQ(submitted_again.request_id, 72U);
    EXPECT_TRUE(std::holds_alternative<protocol::TelemetryAccepted>(submitted_again.body));

    const auto queried = make_processing_response(query_envelope("device", "temperature"), processor);
    ASSERT_EQ(queried.request_id, 73U);
    ASSERT_TRUE(std::holds_alternative<protocol::Statistics>(queried.body));
    const auto& stats = std::get<protocol::Statistics>(queried.body);
    EXPECT_EQ(stats.metric, "temperature");
    EXPECT_EQ(stats.sample_count, 2U);
    EXPECT_DOUBLE_EQ(stats.minimum, 2.0);
    EXPECT_DOUBLE_EQ(stats.maximum, 8.0);
    EXPECT_DOUBLE_EQ(stats.mean, 5.0);
    EXPECT_DOUBLE_EQ(stats.last_value, 8.0);
    EXPECT_EQ(stats.last_received_at, "1970-01-01T00:00:00.456Z");
}

TEST(ProcessingResponse, MapsDomainFailuresToApprovedWireCodes)
{
    domain::TelemetryStore store{1U};
    domain::TelemetryProcessor processor{store};
    ASSERT_TRUE(std::holds_alternative<protocol::TelemetryAccepted>(
        make_processing_response(submit_envelope("device", "m", 1.0), processor).body));

    const auto invalid = make_processing_response(submit_envelope("", "m", 1.0), processor);
    ASSERT_TRUE(std::holds_alternative<ErrorResponse>(invalid.body));
    EXPECT_EQ(error_of(invalid).code, "INVALID_FIELD_VALUE");

    const auto missing = make_processing_response(query_envelope("device", "missing"), processor);
    ASSERT_TRUE(std::holds_alternative<ErrorResponse>(missing.body));
    EXPECT_EQ(error_of(missing).code, "STATISTICS_NOT_FOUND");

    const auto capacity = make_processing_response(submit_envelope("another", "m", 2.0), processor);
    ASSERT_TRUE(std::holds_alternative<ErrorResponse>(capacity.body));
    EXPECT_EQ(error_of(capacity).code, "PROCESSING_UNAVAILABLE");
}

} // namespace
} // namespace telemetryhub::server
