#include <telemetryhub/network/winsock_error.hpp>
#include <telemetryhub/network/winsock_runtime.hpp>

#include <gtest/gtest.h>

#include <winsock2.h>

#include <string>
#include <type_traits>

namespace {

using telemetryhub::network::WinsockRuntime;
using telemetryhub::network::make_winsock_error_code;
using telemetryhub::network::winsock_error_category;

static_assert(!std::is_copy_constructible_v<WinsockRuntime>);
static_assert(!std::is_copy_assignable_v<WinsockRuntime>);
static_assert(!std::is_move_constructible_v<WinsockRuntime>);
static_assert(!std::is_move_assignable_v<WinsockRuntime>);
static_assert(std::is_nothrow_destructible_v<WinsockRuntime>);

TEST(WinsockRuntime, InitializesWinsockSuccessfully)
{
    const WinsockRuntime runtime;

    const SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(handle, INVALID_SOCKET);
    EXPECT_EQ(closesocket(handle), 0);
}

TEST(WinsockRuntime, SupportsIndependentNestedInstances)
{
    const WinsockRuntime outer_runtime;
    {
        const WinsockRuntime inner_runtime;
        const SOCKET inner_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_NE(inner_handle, INVALID_SOCKET);
        ASSERT_EQ(closesocket(inner_handle), 0);
    }

    const SOCKET outer_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(outer_handle, INVALID_SOCKET);
    EXPECT_EQ(closesocket(outer_handle), 0);
}

TEST(WinsockError, ConstructsKnownErrorCode)
{
    const std::error_code error = make_winsock_error_code(WSAENOTSOCK);

    EXPECT_EQ(error.value(), WSAENOTSOCK);
    EXPECT_EQ(&error.category(), &winsock_error_category());
    EXPECT_STREQ(error.category().name(), "winsock");
    EXPECT_EQ(error.message(), "Not a socket");
}

TEST(WinsockError, ProvidesSafeUnknownErrorMessage)
{
    constexpr int unknown_error = 123456;
    const std::error_code error = make_winsock_error_code(unknown_error);

    EXPECT_NE(error.message().find(std::to_string(unknown_error)), std::string::npos);
}

} // namespace
