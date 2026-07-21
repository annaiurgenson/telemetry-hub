function(telemetryhub_set_project_warnings target_name)
    if(MSVC)
        target_compile_options(
            ${target_name}
            INTERFACE
                /W4
                /WX
                /permissive-
                /Zc:preprocessor
        )
    else()
        message(WARNING "TelemetryHub currently supports MSVC only; strict warnings are not configured for this compiler")
    endif()
endfunction()

