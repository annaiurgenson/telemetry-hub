#include <telemetryhub/server/processing_response.hpp>

#include <chrono>
#include <iomanip>
#include <locale>
#include <sstream>
#include <type_traits>
#include <utility>
#include <variant>

namespace telemetryhub::server {
namespace {

using domain::TelemetryProcessingError;
using protocol::ErrorResponse;
using protocol::Response;
using protocol::ResponseBody;

[[nodiscard]] Response error_response(
    const protocol::RequestId request_id,
    const char* const code,
    const char* const message)
{
    return Response{request_id, ResponseBody{ErrorResponse{code, message}}};
}

[[nodiscard]] Response error_response(
    const protocol::RequestId request_id,
    const TelemetryProcessingError error)
{
    switch (error)
    {
    case TelemetryProcessingError::invalid_input:
        return error_response(request_id, "INVALID_FIELD_VALUE", "The command contains an invalid value");
    case TelemetryProcessingError::statistics_not_found:
        return error_response(request_id, "STATISTICS_NOT_FOUND", "No statistics are available for this metric");
    case TelemetryProcessingError::capacity_reached:
        return error_response(request_id, "PROCESSING_UNAVAILABLE", "Telemetry storage capacity is reached");
    case TelemetryProcessingError::count_overflow:
        return error_response(request_id, "PROCESSING_UNAVAILABLE", "Telemetry statistics cannot be updated");
    case TelemetryProcessingError::numeric_failure:
        return error_response(request_id, "PROCESSING_UNAVAILABLE", "Telemetry statistics cannot be updated");
    }
    return error_response(request_id, "PROCESSING_UNAVAILABLE", "Command processing failed");
}

} // namespace

std::optional<std::string> format_received_timestamp(const domain::ReceivedAt received_at)
{
    using namespace std::chrono;

    const auto day_point = floor<days>(received_at);
    const year_month_day date{day_point};
    const int year = static_cast<int>(date.year());
    if (!date.ok() || year < 0 || year > 9'999)
    {
        return std::nullopt;
    }

    const auto milliseconds_since_midnight =
        floor<milliseconds>(received_at - day_point).count();
    const auto hours = milliseconds_since_midnight / 3'600'000;
    const auto minutes = (milliseconds_since_midnight / 60'000) % 60;
    const auto seconds = (milliseconds_since_midnight / 1'000) % 60;
    const auto milliseconds = milliseconds_since_midnight % 1'000;

    std::ostringstream timestamp;
    timestamp.imbue(std::locale::classic());
    timestamp << std::setfill('0') << std::setw(4) << year << '-'
              << std::setw(2) << static_cast<unsigned int>(date.month()) << '-'
              << std::setw(2) << static_cast<unsigned int>(date.day()) << 'T'
              << std::setw(2) << hours << ':'
              << std::setw(2) << minutes << ':'
              << std::setw(2) << seconds << '.'
              << std::setw(3) << milliseconds << 'Z';
    return timestamp.str();
}

Response make_processing_response(
    const CommandEnvelope& envelope,
    const domain::TelemetryProcessor& processor)
{
    return std::visit(
        [&envelope, &processor](const auto& command) -> Response {
            using Command = std::decay_t<decltype(command)>;
            if constexpr (std::is_same_v<Command, protocol::SubmitTelemetry>)
            {
                const auto result = processor.submit_telemetry(
                    envelope.device_id, command.metric, command.value,
                    envelope.received_at, envelope.receive_sequence);
                if (std::holds_alternative<domain::TelemetryAcceptedResult>(result))
                {
                    return Response{envelope.request_id, ResponseBody{protocol::TelemetryAccepted{}}};
                }
                return error_response(
                    envelope.request_id, std::get<TelemetryProcessingError>(result));
            }
            else
            {
                const auto result = processor.get_statistics(envelope.device_id, command.metric);
                if (const auto* error = std::get_if<TelemetryProcessingError>(&result))
                {
                    return error_response(envelope.request_id, *error);
                }

                const auto& query = std::get<domain::StatisticsQueryResult>(result);
                const auto formatted_timestamp = format_received_timestamp(
                    query.statistics.last_received_at);
                if (!formatted_timestamp.has_value())
                {
                    return make_processing_unavailable_response(envelope.request_id);
                }

                return Response{
                    envelope.request_id,
                    ResponseBody{protocol::Statistics{
                        query.metric,
                        query.statistics.sample_count,
                        query.statistics.minimum,
                        query.statistics.maximum,
                        query.statistics.mean,
                        query.statistics.last_value,
                        *formatted_timestamp}}};
            }
        },
        envelope.command);
}

Response make_processing_unavailable_response(const protocol::RequestId request_id)
{
    return error_response(request_id, "PROCESSING_UNAVAILABLE", "Command processing is unavailable");
}

} // namespace telemetryhub::server
