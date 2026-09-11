if(NOT DEFINED SEMILIVE_PUBLISHER)
    message(FATAL_ERROR "SEMILIVE_PUBLISHER is required")
endif()

execute_process(
    COMMAND "${SEMILIVE_PUBLISHER}" --version
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error
)
if(NOT version_result EQUAL 0 OR
   NOT version_output MATCHES "semilive_publisher 0\\.1\\.0-dev")
    message(FATAL_ERROR
        "publisher --version failed: result=${version_result}, "
        "stdout=${version_output}, stderr=${version_error}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_PUBLISHER}" --output
    RESULT_VARIABLE output_result
    OUTPUT_VARIABLE output_stdout
    ERROR_VARIABLE output_stderr
)
if(output_result EQUAL 0 OR
   NOT output_stderr MATCHES "--output requires a path")
    message(FATAL_ERROR
        "publisher accepted a missing output path: result=${output_result}, "
        "stdout=${output_stdout}, stderr=${output_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_PUBLISHER}" --display invalid
    RESULT_VARIABLE display_result
    OUTPUT_VARIABLE display_stdout
    ERROR_VARIABLE display_stderr
)
if(display_result EQUAL 0 OR
   NOT display_stderr MATCHES
       "--display requires primary or a zero-based index")
    message(FATAL_ERROR
        "publisher accepted an invalid display: result=${display_result}, "
        "stdout=${display_stdout}, stderr=${display_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_PUBLISHER}" --help --no-pointer
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_stdout
    ERROR_VARIABLE help_stderr
)
if(help_result EQUAL 0 OR
   NOT help_stderr MATCHES "--help cannot be combined")
    message(FATAL_ERROR
        "publisher accepted combined help arguments: result=${help_result}, "
        "stdout=${help_stdout}, stderr=${help_stderr}"
    )
endif()
