#include "command_line.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace {

using device_simulator::ParseResult;
using device_simulator::ParseStatus;

[[nodiscard]] ParseResult parse(const std::initializer_list<const char*> arguments)
{
    std::vector<const char*> argv{"device_simulator"};
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    return device_simulator::parse_command_line(static_cast<int>(argv.size()), argv.data());
}

TEST(DeviceSimulatorCli, RequiresPortAndAcceptsBoundaryPorts)
{
    const char* const missing_port[]{"device_simulator"};
    EXPECT_EQ(device_simulator::parse_command_line(1, missing_port).status, ParseStatus::error);

    const char* const minimum[]{"device_simulator", "--port", "1"};
    const ParseResult min_result = device_simulator::parse_command_line(3, minimum);
    ASSERT_EQ(min_result.status, ParseStatus::success);
    EXPECT_EQ(min_result.port, 1U);
    EXPECT_FALSE(min_result.interval_ms.has_value());
    EXPECT_FALSE(min_result.count.has_value());

    const char* const maximum[]{"device_simulator", "--port", "65535"};
    const ParseResult max_result = device_simulator::parse_command_line(3, maximum);
    ASSERT_EQ(max_result.status, ParseStatus::success);
    EXPECT_EQ(max_result.port, 65'535U);
}

TEST(DeviceSimulatorCli, HelpMustBeUsedAlone)
{
    const char* const help_argv[]{"device_simulator", "--help"};
    EXPECT_EQ(device_simulator::parse_command_line(2, help_argv).status, ParseStatus::help);

    const char* const combined_argv[]{"device_simulator", "--help", "--port", "5000"};
    const ParseResult combined = device_simulator::parse_command_line(4, combined_argv);
    EXPECT_EQ(combined.status, ParseStatus::error);
    EXPECT_EQ(combined.diagnostic, "--help must be used by itself");
}

TEST(DeviceSimulatorCli, RejectsInvalidAndDuplicateArguments)
{
    const char* const zero_port[]{"device_simulator", "--port", "0"};
    EXPECT_EQ(device_simulator::parse_command_line(3, zero_port).status, ParseStatus::error);

    const char* const high_port[]{"device_simulator", "--port", "65536"};
    EXPECT_EQ(device_simulator::parse_command_line(3, high_port).status, ParseStatus::error);

    const char* const malformed[]{"device_simulator", "--port", "50x"};
    EXPECT_EQ(device_simulator::parse_command_line(3, malformed).status, ParseStatus::error);

    const char* const duplicate[]{"device_simulator", "--port", "5000", "--port", "5001"};
    const ParseResult duplicate_result = device_simulator::parse_command_line(5, duplicate);
    EXPECT_EQ(duplicate_result.status, ParseStatus::error);
    EXPECT_EQ(duplicate_result.diagnostic, "option specified more than once");

    const char* const unknown[]{"device_simulator", "--host", "localhost"};
    EXPECT_EQ(device_simulator::parse_command_line(3, unknown).status, ParseStatus::error);
}

TEST(DeviceSimulatorCli, UsageDescribesLocalhostAndPortRange)
{
    const auto usage = device_simulator::usage_text();
    EXPECT_NE(usage.find("--port"), decltype(usage)::npos);
    EXPECT_NE(usage.find("127.0.0.1"), decltype(usage)::npos);
    EXPECT_NE(usage.find("1..65535"), decltype(usage)::npos);
    EXPECT_NE(usage.find("--interval-ms <1..60000>"), decltype(usage)::npos);
    EXPECT_NE(usage.find("--count <1..1000>"), decltype(usage)::npos);
    EXPECT_NE(usage.find("requires both"), decltype(usage)::npos);
}

TEST(DeviceSimulatorCli, ParsesPeriodicOptionsInEitherOrderAndAtTheirBoundaries)
{
    const ParseResult minimum = parse(
        {"--port", "1", "--interval-ms", "1", "--count", "1"});
    ASSERT_EQ(minimum.status, ParseStatus::success);
    EXPECT_EQ(minimum.port, 1U);
    EXPECT_EQ(minimum.interval_ms, 1U);
    EXPECT_EQ(minimum.count, 1U);

    const ParseResult maximum = parse(
        {"--count", "1000", "--port", "65535", "--interval-ms", "60000"});
    ASSERT_EQ(maximum.status, ParseStatus::success);
    EXPECT_EQ(maximum.port, 65'535U);
    EXPECT_EQ(maximum.interval_ms, 60'000U);
    EXPECT_EQ(maximum.count, 1'000U);
}

TEST(DeviceSimulatorCli, RequiresBothPeriodicOptionsAndRejectsInvalidValues)
{
    EXPECT_EQ(parse({"--port", "5052", "--interval-ms", "10"}).status, ParseStatus::error);
    EXPECT_EQ(parse({"--port", "5052", "--count", "3"}).status, ParseStatus::error);

    EXPECT_EQ(
        parse({"--port", "5052", "--interval-ms", "0", "--count", "3"}).status,
        ParseStatus::error);
    EXPECT_EQ(
        parse({"--port", "5052", "--interval-ms", "60001", "--count", "3"}).status,
        ParseStatus::error);
    EXPECT_EQ(
        parse({"--port", "5052", "--interval-ms", "5ms", "--count", "3"}).status,
        ParseStatus::error);
    EXPECT_EQ(
        parse({"--port", "5052", "--interval-ms", "10", "--count", "0"}).status,
        ParseStatus::error);
    EXPECT_EQ(
        parse({"--port", "5052", "--interval-ms", "10", "--count", "1001"}).status,
        ParseStatus::error);
    EXPECT_EQ(
        parse({"--port", "5052", "--interval-ms", "10", "--count", "4294967296"}).status,
        ParseStatus::error);
    EXPECT_EQ(
        parse({"--port", "5052", "--interval-ms", "10", "--count", "3", "--count", "4"})
            .status,
        ParseStatus::error);
    EXPECT_EQ(
        parse({"--port", "5052", "--interval-ms", "10", "--count", "3", "--interval-ms", "20"})
            .status,
        ParseStatus::error);
}

} // namespace
