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

add_executable(telemetry_server apps/telemetry_server/main.cpp)
target_link_libraries(telemetry_server PRIVATE telemetryhub_project_options)

add_executable(device_simulator apps/device_simulator/main.cpp)
target_link_libraries(device_simulator PRIVATE telemetryhub_project_options)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

