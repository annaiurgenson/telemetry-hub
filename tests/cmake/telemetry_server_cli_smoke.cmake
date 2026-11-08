if(NOT DEFINED TELEMETRY_SERVER OR NOT DEFINED TEST_MODE)
    message(FATAL_ERROR "TELEMETRY_SERVER and TEST_MODE are required")
endif()

if(TEST_MODE STREQUAL "help")
    execute_process(
        COMMAND "${TELEMETRY_SERVER}" --help
        RESULT_VARIABLE exit_code
        OUTPUT_VARIABLE standard_output
        ERROR_VARIABLE standard_error
    )

    if(NOT "${exit_code}" STREQUAL "0")
        message(FATAL_ERROR "--help returned ${exit_code}; stderr: ${standard_error}")
    endif()
    if(NOT standard_output MATCHES "Usage: telemetry_server")
        message(FATAL_ERROR "--help did not print usage; stdout: ${standard_output}")
    endif()
    if(NOT "${standard_error}" STREQUAL "")
        message(FATAL_ERROR "--help unexpectedly wrote to stderr: ${standard_error}")
    endif()
elseif(TEST_MODE STREQUAL "invalid")
    execute_process(
        COMMAND "${TELEMETRY_SERVER}" --port 65536
        RESULT_VARIABLE exit_code
        OUTPUT_VARIABLE standard_output
        ERROR_VARIABLE standard_error
    )

    if(NOT "${exit_code}" STREQUAL "2")
        message(FATAL_ERROR "invalid CLI input returned ${exit_code}; stderr: ${standard_error}")
    endif()
    if(NOT standard_error MATCHES "--port value must be in range")
        message(FATAL_ERROR "invalid CLI input did not explain the port range: ${standard_error}")
    endif()
    if(NOT standard_output STREQUAL "")
        message(FATAL_ERROR "invalid CLI input unexpectedly wrote to stdout: ${standard_output}")
    endif()
else()
    message(FATAL_ERROR "Unsupported TEST_MODE: ${TEST_MODE}")
endif()
