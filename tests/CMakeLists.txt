add_test(NAME semilive_publisher_help COMMAND semilive_publisher --help)
add_test(NAME semilive_relay_help COMMAND semilive_relay --help)
add_test(NAME semilive_receiver_help COMMAND semilive_receiver --help)

add_subdirectory(common)
add_subdirectory(publisher)
