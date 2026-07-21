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
gtest_discover_tests(telemetryhub_concurrency_tests)

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
gtest_discover_tests(telemetryhub_worker_pool_tests)
