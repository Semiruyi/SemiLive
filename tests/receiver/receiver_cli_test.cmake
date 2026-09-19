if(NOT DEFINED SEMILIVE_RECEIVER)
    message(FATAL_ERROR "SEMILIVE_RECEIVER is required")
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}" --version
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error
)
if(NOT version_result EQUAL 0 OR
   NOT version_output MATCHES "semilive_receiver 0\\.1\\.0-dev")
    message(FATAL_ERROR
        "receiver --version failed: result=${version_result}, "
        "stdout=${version_output}, stderr=${version_error}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}" --bind-port 0
    RESULT_VARIABLE port_result
    OUTPUT_VARIABLE port_stdout
    ERROR_VARIABLE port_stderr
)
if(port_result EQUAL 0 OR
   NOT port_stderr MATCHES
       "--bind-port requires an integer in 1..65535")
    message(FATAL_ERROR
        "receiver accepted invalid bind port: result=${port_result}, "
        "stdout=${port_stdout}, stderr=${port_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}" --rtp-payload-type 128
    RESULT_VARIABLE payload_type_result
    OUTPUT_VARIABLE payload_type_stdout
    ERROR_VARIABLE payload_type_stderr
)
if(payload_type_result EQUAL 0 OR
   NOT payload_type_stderr MATCHES
       "--rtp-payload-type requires an integer in 96..127")
    message(FATAL_ERROR
        "receiver accepted invalid payload type: "
        "result=${payload_type_result}, stdout=${payload_type_stdout}, "
        "stderr=${payload_type_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}" --rtp-ssrc 4294967296
    RESULT_VARIABLE ssrc_result
    OUTPUT_VARIABLE ssrc_stdout
    ERROR_VARIABLE ssrc_stderr
)
if(ssrc_result EQUAL 0 OR
   NOT ssrc_stderr MATCHES
       "--rtp-ssrc requires an integer in 0..4294967295")
    message(FATAL_ERROR
        "receiver accepted invalid SSRC: result=${ssrc_result}, "
        "stdout=${ssrc_stdout}, stderr=${ssrc_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}" --rtp-max-datagram-bytes 14
    RESULT_VARIABLE datagram_result
    OUTPUT_VARIABLE datagram_stdout
    ERROR_VARIABLE datagram_stderr
)
if(datagram_result EQUAL 0 OR
   NOT datagram_stderr MATCHES
       "--rtp-max-datagram-bytes requires an integer in 15..65507")
    message(FATAL_ERROR
        "receiver accepted invalid datagram size: result=${datagram_result}, "
        "stdout=${datagram_stdout}, stderr=${datagram_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}" --poll-interval-ms 0
    RESULT_VARIABLE poll_result
    OUTPUT_VARIABLE poll_stdout
    ERROR_VARIABLE poll_stderr
)
if(poll_result EQUAL 0 OR
   NOT poll_stderr MATCHES
       "--poll-interval-ms requires an integer in 1..1000")
    message(FATAL_ERROR
        "receiver accepted invalid poll interval: result=${poll_result}, "
        "stdout=${poll_stdout}, stderr=${poll_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}" --udp-receive-buffer-bytes 0
    RESULT_VARIABLE receive_buffer_result
    OUTPUT_VARIABLE receive_buffer_stdout
    ERROR_VARIABLE receive_buffer_stderr
)
if(receive_buffer_result EQUAL 0 OR
   NOT receive_buffer_stderr MATCHES
       "--udp-receive-buffer-bytes requires an integer in 1..2147483647")
    message(FATAL_ERROR
        "receiver accepted invalid UDP receive buffer: "
        "result=${receive_buffer_result}, stdout=${receive_buffer_stdout}, "
        "stderr=${receive_buffer_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}" --output
    RESULT_VARIABLE output_result
    OUTPUT_VARIABLE output_stdout
    ERROR_VARIABLE output_stderr
)
if(output_result EQUAL 0 OR
   NOT output_stderr MATCHES "--output requires a path")
    message(FATAL_ERROR
        "receiver accepted missing output path: result=${output_result}, "
        "stdout=${output_stdout}, stderr=${output_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}" --stats-json
    RESULT_VARIABLE stats_path_result
    OUTPUT_VARIABLE stats_path_stdout
    ERROR_VARIABLE stats_path_stderr
)
if(stats_path_result EQUAL 0 OR
   NOT stats_path_stderr MATCHES "--stats-json requires a path")
    message(FATAL_ERROR
        "receiver accepted missing stats path: result=${stats_path_result}, "
        "stdout=${stats_path_stdout}, stderr=${stats_path_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}" --output-stall-threshold-ms 0
    RESULT_VARIABLE stall_threshold_result
    OUTPUT_VARIABLE stall_threshold_stdout
    ERROR_VARIABLE stall_threshold_stderr
)
if(stall_threshold_result EQUAL 0 OR
   NOT stall_threshold_stderr MATCHES
       "--output-stall-threshold-ms requires an integer in 1..60000")
    message(FATAL_ERROR
        "receiver accepted invalid output stall threshold: "
        "result=${stall_threshold_result}, stdout=${stall_threshold_stdout}, "
        "stderr=${stall_threshold_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}"
            --stats-json first.json --stats-json second.json
    RESULT_VARIABLE duplicate_stats_result
    OUTPUT_VARIABLE duplicate_stats_stdout
    ERROR_VARIABLE duplicate_stats_stderr
)
if(duplicate_stats_result EQUAL 0 OR
   NOT duplicate_stats_stderr MATCHES
       "--stats-json may only be specified once")
    message(FATAL_ERROR
        "receiver accepted duplicate stats path: "
        "result=${duplicate_stats_result}, stdout=${duplicate_stats_stdout}, "
        "stderr=${duplicate_stats_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}"
            --output same-output --stats-json same-output
    RESULT_VARIABLE same_output_result
    OUTPUT_VARIABLE same_output_stdout
    ERROR_VARIABLE same_output_stderr
)
if(same_output_result EQUAL 0 OR
   NOT same_output_stderr MATCHES
       "--stats-json must not use the H.264 output path")
    message(FATAL_ERROR
        "receiver accepted overlapping media and stats paths: "
        "result=${same_output_result}, stdout=${same_output_stdout}, "
        "stderr=${same_output_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}" --bind-port 5004 --bind-port 5005
    RESULT_VARIABLE duplicate_result
    OUTPUT_VARIABLE duplicate_stdout
    ERROR_VARIABLE duplicate_stderr
)
if(duplicate_result EQUAL 0 OR
   NOT duplicate_stderr MATCHES
       "--bind-port may only be specified once")
    message(FATAL_ERROR
        "receiver accepted duplicate bind port: result=${duplicate_result}, "
        "stdout=${duplicate_stdout}, stderr=${duplicate_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}" --help --bind-port 5004
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_stdout
    ERROR_VARIABLE help_stderr
)
if(help_result EQUAL 0 OR
   NOT help_stderr MATCHES "--help cannot be combined")
    message(FATAL_ERROR
        "receiver accepted combined help arguments: result=${help_result}, "
        "stdout=${help_stdout}, stderr=${help_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RECEIVER}" --unknown
    RESULT_VARIABLE unknown_result
    OUTPUT_VARIABLE unknown_stdout
    ERROR_VARIABLE unknown_stderr
)
if(unknown_result EQUAL 0 OR
   NOT unknown_stderr MATCHES "unknown argument: --unknown")
    message(FATAL_ERROR
        "receiver accepted unknown argument: result=${unknown_result}, "
        "stdout=${unknown_stdout}, stderr=${unknown_stderr}"
    )
endif()
