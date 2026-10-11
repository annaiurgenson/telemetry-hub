cmake_minimum_required(VERSION 3.25)

project(
    TelemetryHub
    VERSION 0.1.0
    DESCRIPTION "A bounded multithreaded telemetry server"
    LANGUAGES CXX
)

include(CTest)
include(cmake/CompilerWarnings.cmake)

add_library(telemetryhub_project_options INTERFACE)
target_compile_features(telemetryhub_project_options INTERFACE cxx_std_20)
telemetryhub_set_project_warnings(telemetryhub_project_options)

add_library(
    telemetryhub_concurrency
    src/concurrency/worker_pool.cpp
)
target_include_directories(
    telemetryhub_concurrency
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(telemetryhub_concurrency PUBLIC telemetryhub_project_options)

add_library(
    telemetryhub_network
    src/network/socket.cpp
    src/network/tcp.cpp
    src/network/winsock_error.cpp
    src/network/winsock_runtime.cpp
)
target_include_directories(
    telemetryhub_network
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(
    telemetryhub_network
    PUBLIC
        telemetryhub_project_options
    PRIVATE
        ws2_32
)

add_library(
    telemetryhub_protocol
    src/protocol/message_framer.cpp
)
target_include_directories(
    telemetryhub_protocol
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(telemetryhub_protocol PUBLIC telemetryhub_project_options)

add_executable(telemetry_server apps/telemetry_server/main.cpp)
target_link_libraries(telemetry_server PRIVATE telemetryhub_project_options)

add_executable(device_simulator apps/device_simulator/main.cpp)
target_link_libraries(device_simulator PRIVATE telemetryhub_project_options)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
