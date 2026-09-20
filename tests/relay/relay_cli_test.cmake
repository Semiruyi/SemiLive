if(NOT DEFINED SEMILIVE_RELAY)
    message(FATAL_ERROR "SEMILIVE_RELAY is required")
endif()

execute_process(
    COMMAND "${SEMILIVE_RELAY}" --version
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error
)
if(NOT version_result EQUAL 0 OR
   NOT version_output MATCHES "semilive_relay 0\\.1\\.0-dev")
    message(FATAL_ERROR
        "relay --version failed: result=${version_result}, "
        "stdout=${version_output}, stderr=${version_error}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RELAY}"
    RESULT_VARIABLE endpoint_result
    OUTPUT_VARIABLE endpoint_stdout
    ERROR_VARIABLE endpoint_stderr
)
if(endpoint_result EQUAL 0 OR
   NOT endpoint_stderr MATCHES
       "--bind-port and --forward-port are required")
    message(FATAL_ERROR
        "relay accepted missing endpoints: result=${endpoint_result}, "
        "stdout=${endpoint_stdout}, stderr=${endpoint_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RELAY}"
            --bind-port 0 --forward-port 5006
    RESULT_VARIABLE bind_port_result
    OUTPUT_VARIABLE bind_port_stdout
    ERROR_VARIABLE bind_port_stderr
)
if(bind_port_result EQUAL 0 OR
   NOT bind_port_stderr MATCHES
       "--bind-port requires an integer in 1..65535")
    message(FATAL_ERROR
        "relay accepted invalid bind port: result=${bind_port_result}, "
        "stdout=${bind_port_stdout}, stderr=${bind_port_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RELAY}"
            --bind-port 5004 --forward-port 5006
            --loss-percent 100.0001
    RESULT_VARIABLE loss_result
    OUTPUT_VARIABLE loss_stdout
    ERROR_VARIABLE loss_stderr
)
if(loss_result EQUAL 0 OR
   NOT loss_stderr MATCHES
       "--loss-percent requires a decimal in 0..100")
    message(FATAL_ERROR
        "relay accepted invalid loss rate: result=${loss_result}, "
        "stdout=${loss_stdout}, stderr=${loss_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RELAY}"
            --bind-port 5004 --forward-port 5004
    RESULT_VARIABLE loop_result
    OUTPUT_VARIABLE loop_stdout
    ERROR_VARIABLE loop_stderr
)
if(loop_result EQUAL 0 OR
   NOT loop_stderr MATCHES
       "input and forward UDP endpoints must be different")
    message(FATAL_ERROR
        "relay accepted a self-loop: result=${loop_result}, "
        "stdout=${loop_stdout}, stderr=${loop_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RELAY}"
            --bind-port 5004 --bind-port 5005 --forward-port 5006
    RESULT_VARIABLE duplicate_result
    OUTPUT_VARIABLE duplicate_stdout
    ERROR_VARIABLE duplicate_stderr
)
if(duplicate_result EQUAL 0 OR
   NOT duplicate_stderr MATCHES
       "--bind-port may only be specified once")
    message(FATAL_ERROR
        "relay accepted duplicate bind port: result=${duplicate_result}, "
        "stdout=${duplicate_stdout}, stderr=${duplicate_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RELAY}" --stats-json
    RESULT_VARIABLE stats_result
    OUTPUT_VARIABLE stats_stdout
    ERROR_VARIABLE stats_stderr
)
if(stats_result EQUAL 0 OR
   NOT stats_stderr MATCHES "--stats-json requires a value")
    message(FATAL_ERROR
        "relay accepted missing stats path: result=${stats_result}, "
        "stdout=${stats_stdout}, stderr=${stats_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RELAY}" --help --bind-port 5004
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_stdout
    ERROR_VARIABLE help_stderr
)
if(help_result EQUAL 0 OR
   NOT help_stderr MATCHES "--help cannot be combined")
    message(FATAL_ERROR
        "relay accepted combined help arguments: result=${help_result}, "
        "stdout=${help_stdout}, stderr=${help_stderr}"
    )
endif()

execute_process(
    COMMAND "${SEMILIVE_RELAY}" --unknown
    RESULT_VARIABLE unknown_result
    OUTPUT_VARIABLE unknown_stdout
    ERROR_VARIABLE unknown_stderr
)
if(unknown_result EQUAL 0 OR
   NOT unknown_stderr MATCHES "unknown argument: --unknown")
    message(FATAL_ERROR
        "relay accepted unknown argument: result=${unknown_result}, "
        "stdout=${unknown_stdout}, stderr=${unknown_stderr}"
    )
endif()
