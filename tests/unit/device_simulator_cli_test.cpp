#include "command_line.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using device_simulator::ParseResult;
using device_simulator::ParseStatus;

TEST(DeviceSimulatorCli, RequiresPortAndAcceptsBoundaryPorts)
{
    const char* const missing_port[]{"device_simulator"};
    EXPECT_EQ(device_simulator::parse_command_line(1, missing_port).status, ParseStatus::error);

    const char* const minimum[]{"device_simulator", "--port", "1"};
    const ParseResult min_result = device_simulator::parse_command_line(3, minimum);
    ASSERT_EQ(min_result.status, ParseStatus::success);
    EXPECT_EQ(min_result.port, 1U);

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
}

} // namespace
