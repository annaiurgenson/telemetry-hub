#include <telemetryhub/network/socket.hpp>
#include <telemetryhub/network/winsock_runtime.hpp>

#include <gtest/gtest.h>

#include <winsock2.h>

#include <system_error>
#include <type_traits>
#include <utility>

namespace {

using telemetryhub::network::Socket;
using telemetryhub::network::WinsockRuntime;

static_assert(!std::is_copy_constructible_v<Socket>);
static_assert(!std::is_copy_assignable_v<Socket>);
static_assert(std::is_nothrow_move_constructible_v<Socket>);
static_assert(std::is_move_assignable_v<Socket>);
static_assert(std::is_nothrow_destructible_v<Socket>);

class SocketTest : public testing::Test
{
protected:
    [[nodiscard]] static SOCKET create_native_socket()
    {
        return socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }

    static void expect_handle_is_closed(const SOCKET handle)
    {
        EXPECT_EQ(closesocket(handle), SOCKET_ERROR);
        EXPECT_EQ(WSAGetLastError(), WSAENOTSOCK);
    }

    WinsockRuntime runtime_;
};

TEST_F(SocketTest, DefaultConstructsInvalid)
{
    const Socket socket;

    EXPECT_FALSE(socket.valid());
    EXPECT_EQ(socket.native_handle(), INVALID_SOCKET);
}

TEST_F(SocketTest, TakesOwnershipOfValidNativeSocket)
{
    const SOCKET handle = create_native_socket();
    ASSERT_NE(handle, INVALID_SOCKET);
    const Socket socket{handle};

    EXPECT_TRUE(socket.valid());
    EXPECT_EQ(socket.native_handle(), handle);
}

TEST_F(SocketTest, ClosesOwnedSocketOnDestruction)
{
    const SOCKET handle = create_native_socket();
    ASSERT_NE(handle, INVALID_SOCKET);
    {
        const Socket socket{handle};
        ASSERT_TRUE(socket.valid());
    }

    expect_handle_is_closed(handle);
}

TEST_F(SocketTest, MoveConstructionTransfersOwnership)
{
    const SOCKET handle = create_native_socket();
    ASSERT_NE(handle, INVALID_SOCKET);
    Socket source{handle};

    const Socket destination{std::move(source)};

    EXPECT_FALSE(source.valid());
    EXPECT_EQ(source.native_handle(), INVALID_SOCKET);
    EXPECT_TRUE(destination.valid());
    EXPECT_EQ(destination.native_handle(), handle);
}

TEST_F(SocketTest, MoveAssignmentClosesPreviousHandleAndTransfersOwnership)
{
    const SOCKET old_handle = create_native_socket();
    const SOCKET new_handle = create_native_socket();
    ASSERT_NE(old_handle, INVALID_SOCKET);
    ASSERT_NE(new_handle, INVALID_SOCKET);
    Socket destination{old_handle};
    Socket source{new_handle};

    destination = std::move(source);

    expect_handle_is_closed(old_handle);
    EXPECT_FALSE(source.valid());
    EXPECT_EQ(destination.native_handle(), new_handle);
}

TEST_F(SocketTest, ReleaseTransfersHandleWithoutClosing)
{
    const SOCKET handle = create_native_socket();
    ASSERT_NE(handle, INVALID_SOCKET);
    Socket socket{handle};

    const SOCKET released = socket.release();

    EXPECT_EQ(released, handle);
    EXPECT_FALSE(socket.valid());
    Socket cleanup{released};
}

TEST_F(SocketTest, ResetClearsAndClosesOwnedHandle)
{
    const SOCKET handle = create_native_socket();
    ASSERT_NE(handle, INVALID_SOCKET);
    Socket socket{handle};

    const std::error_code error = socket.reset();

    EXPECT_FALSE(error);
    EXPECT_FALSE(socket.valid());
    expect_handle_is_closed(handle);
}

TEST_F(SocketTest, RepeatedCloseIsSafe)
{
    const SOCKET handle = create_native_socket();
    ASSERT_NE(handle, INVALID_SOCKET);
    Socket socket{handle};

    EXPECT_FALSE(socket.close());
    EXPECT_FALSE(socket.close());
    EXPECT_FALSE(socket.close());
    EXPECT_FALSE(socket.valid());
}

TEST_F(SocketTest, ResetReplacesAndClosesPreviouslyOwnedHandle)
{
    const SOCKET old_handle = create_native_socket();
    const SOCKET replacement = create_native_socket();
    ASSERT_NE(old_handle, INVALID_SOCKET);
    ASSERT_NE(replacement, INVALID_SOCKET);
    Socket socket{old_handle};

    const std::error_code error = socket.reset(replacement);

    EXPECT_FALSE(error);
    expect_handle_is_closed(old_handle);
    EXPECT_TRUE(socket.valid());
    EXPECT_EQ(socket.native_handle(), replacement);
}

TEST_F(SocketTest, ExplicitCloseReportsSuccess)
{
    const SOCKET handle = create_native_socket();
    ASSERT_NE(handle, INVALID_SOCKET);
    Socket socket{handle};

    const std::error_code error = socket.close();

    EXPECT_FALSE(error);
    EXPECT_FALSE(socket.valid());
    expect_handle_is_closed(handle);
}

TEST_F(SocketTest, FailedExplicitClosePreservesHandleAndReportsWinsockError)
{
    const SOCKET handle = create_native_socket();
    ASSERT_NE(handle, INVALID_SOCKET);
    Socket socket{handle};
    ASSERT_EQ(closesocket(handle), 0);

    const std::error_code error = socket.close();

    EXPECT_EQ(error.value(), WSAENOTSOCK);
    EXPECT_STREQ(error.category().name(), "winsock");
    EXPECT_TRUE(socket.valid());
    EXPECT_EQ(socket.native_handle(), handle);
}

TEST_F(SocketTest, FailedResetDoesNotAdoptReplacement)
{
    const SOCKET stale_handle = create_native_socket();
    const SOCKET replacement = create_native_socket();
    ASSERT_NE(stale_handle, INVALID_SOCKET);
    ASSERT_NE(replacement, INVALID_SOCKET);
    Socket socket{stale_handle};
    Socket replacement_cleanup{replacement};
    ASSERT_EQ(closesocket(stale_handle), 0);

    const std::error_code error = socket.reset(replacement);

    EXPECT_EQ(error.value(), WSAENOTSOCK);
    EXPECT_EQ(socket.native_handle(), stale_handle);
    EXPECT_EQ(replacement_cleanup.native_handle(), replacement);
}

} // namespace
