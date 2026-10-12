include(FetchContent)

set(gtest_force_shared_crt ON CACHE BOOL "Use the shared MSVC runtime for GoogleTest" FORCE)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(telemetryhub_smoke_tests smoke_test.cpp)
target_link_libraries(
    telemetryhub_smoke_tests
    PRIVATE
        telemetryhub_project_options
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(telemetryhub_smoke_tests)

add_executable(
    telemetryhub_concurrency_tests
    unit/bounded_blocking_queue_test.cpp
)
target_link_libraries(
    telemetryhub_concurrency_tests
    PRIVATE
        telemetryhub_project_options
        GTest::gtest_main
)
target_include_directories(
    telemetryhub_concurrency_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)
gtest_discover_tests(telemetryhub_concurrency_tests PROPERTIES TIMEOUT 10)

add_executable(
    telemetryhub_worker_pool_tests
    unit/worker_pool_test.cpp
)
target_link_libraries(
    telemetryhub_worker_pool_tests
    PRIVATE
        telemetryhub_concurrency
        telemetryhub_project_options
        GTest::gtest_main
)
gtest_discover_tests(telemetryhub_worker_pool_tests PROPERTIES TIMEOUT 10)

add_executable(
    telemetryhub_winsock_runtime_tests
    unit/winsock_runtime_test.cpp
)
target_link_libraries(
    telemetryhub_winsock_runtime_tests
    PRIVATE
        telemetryhub_network
        telemetryhub_project_options
        GTest::gtest_main
)
gtest_discover_tests(telemetryhub_winsock_runtime_tests)

add_executable(
    telemetryhub_socket_tests
    unit/socket_test.cpp
)
target_link_libraries(
    telemetryhub_socket_tests
    PRIVATE
        telemetryhub_network
        telemetryhub_project_options
        GTest::gtest_main
)
gtest_discover_tests(telemetryhub_socket_tests)

add_executable(
    telemetryhub_tcp_integration_tests
    integration/tcp_communication_test.cpp
)
target_link_libraries(
    telemetryhub_tcp_integration_tests
    PRIVATE
        telemetryhub_network
        telemetryhub_project_options
        GTest::gtest_main
)
gtest_discover_tests(
    telemetryhub_tcp_integration_tests
    PROPERTIES
        TIMEOUT 10
)

add_executable(
    telemetryhub_message_framer_tests
    unit/message_framer_test.cpp
)
target_link_libraries(
    telemetryhub_message_framer_tests
    PRIVATE
        telemetryhub_protocol
        telemetryhub_project_options
        GTest::gtest_main
)
gtest_discover_tests(telemetryhub_message_framer_tests PROPERTIES TIMEOUT 10)

add_executable(
    telemetryhub_message_codec_tests
    unit/message_codec_test.cpp
)
target_link_libraries(
    telemetryhub_message_codec_tests
    PRIVATE
        telemetryhub_protocol
        telemetryhub_project_options
        GTest::gtest_main
)
gtest_discover_tests(telemetryhub_message_codec_tests PROPERTIES TIMEOUT 10)

add_executable(
    telemetryhub_tcp_server_integration_tests
    integration/tcp_server_test.cpp
)
target_link_libraries(
    telemetryhub_tcp_server_integration_tests
    PRIVATE
        telemetryhub_network
        telemetryhub_project_options
        GTest::gtest_main
)
gtest_discover_tests(
    telemetryhub_tcp_server_integration_tests
    PROPERTIES
        TIMEOUT 10
)
