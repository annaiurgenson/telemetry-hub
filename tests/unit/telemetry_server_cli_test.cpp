#include "command_line.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using telemetry_server::ParseResult;
using telemetry_server::ParseStatus;

[[nodiscard]] ParseResult parse(const std::initializer_list<std::string_view> arguments)
{
    std::vector<std::string> storage;
    storage.reserve(arguments.size());
    for (const std::string_view argument : arguments)
    {
        storage.emplace_back(argument);
    }

    std::vector<const char*> argv;
    argv.reserve(storage.size() + 1U);
    argv.push_back("telemetry_server");
    for (const std::string& argument : storage)
    {
        argv.push_back(argument.c_str());
    }
    return telemetry_server::parse_command_line(
        static_cast<int>(argv.size()), argv.data());
}

TEST(TelemetryServerCli, NoArgumentsReturnNoOverrides)
{
    const ParseResult result = parse({});

    ASSERT_EQ(result.status, ParseStatus::success);
    EXPECT_FALSE(result.overrides.port.has_value());
    EXPECT_FALSE(result.overrides.max_connections.has_value());
}

TEST(TelemetryServerCli, ParsesExplicitOverridesWithoutFillingDefaults)
{
    const ParseResult result = parse({"--port", "0", "--max-connections", "12"});

    ASSERT_EQ(result.status, ParseStatus::success);
    ASSERT_TRUE(result.overrides.port.has_value());
    EXPECT_EQ(*result.overrides.port, 0U);
    ASSERT_TRUE(result.overrides.max_connections.has_value());
    EXPECT_EQ(*result.overrides.max_connections, 12U);
}

TEST(TelemetryServerCli, AcceptsPortAndConnectionCountBoundaries)
{
    const ParseResult port_boundary = parse({"--port", "65535"});
    ASSERT_EQ(port_boundary.status, ParseStatus::success);
    ASSERT_TRUE(port_boundary.overrides.port.has_value());
    EXPECT_EQ(*port_boundary.overrides.port, 65'535U);

    const std::string maximum_connections = std::to_string((std::numeric_limits<int>::max)());
    const ParseResult connection_boundaries = parse({"--max-connections", "1"});
    ASSERT_EQ(connection_boundaries.status, ParseStatus::success);
    ASSERT_TRUE(connection_boundaries.overrides.max_connections.has_value());
    EXPECT_EQ(*connection_boundaries.overrides.max_connections, 1U);

    const ParseResult maximum = parse({"--max-connections", maximum_connections});
    ASSERT_EQ(maximum.status, ParseStatus::success);
    ASSERT_TRUE(maximum.overrides.max_connections.has_value());
    EXPECT_EQ(
        *maximum.overrides.max_connections,
        static_cast<std::size_t>((std::numeric_limits<int>::max)()));
}

TEST(TelemetryServerCli, HelpSucceedsOnlyByItself)
{
    const ParseResult help = parse({"--help"});
    EXPECT_EQ(help.status, ParseStatus::help);

    const ParseResult combined = parse({"--help", "--port", "80"});
    EXPECT_EQ(combined.status, ParseStatus::error);
    EXPECT_EQ(combined.diagnostic, "--help must be used by itself");
}

TEST(TelemetryServerCli, RejectsMissingMalformedAndOutOfRangeValues)
{
    EXPECT_EQ(parse({"--port"}).status, ParseStatus::error);
    EXPECT_EQ(parse({"--max-connections"}).status, ParseStatus::error);
    EXPECT_EQ(parse({"--port", ""}).status, ParseStatus::error);
    EXPECT_EQ(parse({"--port", "12x"}).status, ParseStatus::error);
    EXPECT_EQ(parse({"--port", "-1"}).status, ParseStatus::error);
    EXPECT_EQ(parse({"--port", "65536"}).status, ParseStatus::error);
    EXPECT_EQ(parse({"--max-connections", "0"}).status, ParseStatus::error);

    const auto above_int_max = static_cast<unsigned long long>(
        (std::numeric_limits<int>::max)()) + 1ULL;
    EXPECT_EQ(
        parse({"--max-connections", std::to_string(above_int_max)}).status,
        ParseStatus::error);
    EXPECT_EQ(
        parse({"--port", "18446744073709551616"}).status,
        ParseStatus::error);
}

TEST(TelemetryServerCli, RejectsUnknownAndDuplicateArguments)
{
    const char* const unknown_argv[]{"telemetry_server", "--verbose"};
    const ParseResult unknown = telemetry_server::parse_command_line(2, unknown_argv);
    EXPECT_EQ(unknown.status, ParseStatus::error);
    EXPECT_EQ(unknown.diagnostic, "unknown argument");
    EXPECT_EQ(unknown.argument, "--verbose");

    const char* const duplicate_port_argv[]{
        "telemetry_server", "--port", "80", "--port", "80"};
    const ParseResult duplicate_port = telemetry_server::parse_command_line(
        5, duplicate_port_argv);
    EXPECT_EQ(duplicate_port.status, ParseStatus::error);
    EXPECT_EQ(duplicate_port.diagnostic, "option specified more than once");

    const char* const duplicate_connections_argv[]{
        "telemetry_server", "--max-connections", "1", "--max-connections", "2"};
    const ParseResult duplicate_connections = telemetry_server::parse_command_line(
        5, duplicate_connections_argv);
    EXPECT_EQ(duplicate_connections.status, ParseStatus::error);
    EXPECT_EQ(duplicate_connections.diagnostic, "option specified more than once");
}

TEST(TelemetryServerCli, UsageListsOnlyTheSupportedOptions)
{
    const std::string_view usage = telemetry_server::usage_text();
    EXPECT_NE(usage.find("--port"), std::string_view::npos);
    EXPECT_NE(usage.find("--max-connections"), std::string_view::npos);
    EXPECT_NE(usage.find("--help"), std::string_view::npos);
}

} // namespace
