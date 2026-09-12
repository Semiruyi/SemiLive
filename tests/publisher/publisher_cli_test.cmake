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
    COMMAND "${SEMILIVE_PUBLISHER}"
    RESULT_VARIABLE endpoint_result
    OUTPUT_VARIABLE endpoint_stdout
    ERROR_VARIABLE endpoint_stderr
)
if(endpoint_result EQUAL 0 OR
   NOT endpoint_stderr MATCHES
       "--rtp-address and --rtp-port are required")
    message(FATAL_ERROR
        "publisher accepted a missing RTP endpoint: "
        "result=${endpoint_result}, stdout=${endpoint_stdout}, "
        "stderr=${endpoint_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_PUBLISHER}" --rtp-address 127.0.0.1
    RESULT_VARIABLE missing_port_result
    OUTPUT_VARIABLE missing_port_stdout
    ERROR_VARIABLE missing_port_stderr
)
if(missing_port_result EQUAL 0 OR
   NOT missing_port_stderr MATCHES
       "--rtp-port is required when --rtp-address is specified")
    message(FATAL_ERROR
        "publisher accepted RTP address without port: "
        "result=${missing_port_result}, stdout=${missing_port_stdout}, "
        "stderr=${missing_port_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_PUBLISHER}"
            --rtp-address 127.0.0.1 --rtp-port 70000
    RESULT_VARIABLE invalid_port_result
    OUTPUT_VARIABLE invalid_port_stdout
    ERROR_VARIABLE invalid_port_stderr
)
if(invalid_port_result EQUAL 0 OR
   NOT invalid_port_stderr MATCHES
       "--rtp-port requires an integer in 1..65535")
    message(FATAL_ERROR
        "publisher accepted invalid RTP port: result=${invalid_port_result}, "
        "stdout=${invalid_port_stdout}, stderr=${invalid_port_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_PUBLISHER}"
            --rtp-address 127.0.0.1 --rtp-port 5004
            --rtp-payload-type 95
    RESULT_VARIABLE invalid_pt_result
    OUTPUT_VARIABLE invalid_pt_stdout
    ERROR_VARIABLE invalid_pt_stderr
)
if(invalid_pt_result EQUAL 0 OR
   NOT invalid_pt_stderr MATCHES
       "--rtp-payload-type requires an integer in 96..127")
    message(FATAL_ERROR
        "publisher accepted invalid RTP payload type: "
        "result=${invalid_pt_result}, stdout=${invalid_pt_stdout}, "
        "stderr=${invalid_pt_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_PUBLISHER}"
            --rtp-address 127.0.0.1 --rtp-port 5004
            --rtp-max-datagram-bytes 14
    RESULT_VARIABLE invalid_size_result
    OUTPUT_VARIABLE invalid_size_stdout
    ERROR_VARIABLE invalid_size_stderr
)
if(invalid_size_result EQUAL 0 OR
   NOT invalid_size_stderr MATCHES
       "--rtp-max-datagram-bytes requires an integer in 15..65507")
    message(FATAL_ERROR
        "publisher accepted invalid RTP datagram size: "
        "result=${invalid_size_result}, stdout=${invalid_size_stdout}, "
        "stderr=${invalid_size_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_PUBLISHER}" --output semilive.h264
    RESULT_VARIABLE retired_output_result
    OUTPUT_VARIABLE retired_output_stdout
    ERROR_VARIABLE retired_output_stderr
)
if(retired_output_result EQUAL 0 OR
   NOT retired_output_stderr MATCHES "unknown argument: --output")
    message(FATAL_ERROR
        "publisher accepted retired --output option: "
        "result=${retired_output_result}, stdout=${retired_output_stdout}, "
        "stderr=${retired_output_stderr}"
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
