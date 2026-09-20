add_test(NAME semilive_publisher_help COMMAND semilive_publisher --help)
add_test(
    NAME semilive_publisher_cli
    COMMAND ${CMAKE_COMMAND}
        -DSEMILIVE_PUBLISHER=$<TARGET_FILE:semilive_publisher>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/publisher/publisher_cli_test.cmake
)
add_test(NAME semilive_relay_help COMMAND semilive_relay --help)
add_test(
    NAME semilive_relay_cli
    COMMAND ${CMAKE_COMMAND}
        -DSEMILIVE_RELAY=$<TARGET_FILE:semilive_relay>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/relay/relay_cli_test.cmake
)
add_test(NAME semilive_receiver_help COMMAND semilive_receiver --help)
add_test(
    NAME semilive_receiver_cli
    COMMAND ${CMAKE_COMMAND}
        -DSEMILIVE_RECEIVER=$<TARGET_FILE:semilive_receiver>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/receiver/receiver_cli_test.cmake
)

add_subdirectory(common)
add_subdirectory(publisher)
add_subdirectory(receiver)
add_subdirectory(relay)
