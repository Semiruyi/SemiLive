add_executable(semilive_relay_udp_socket_test
    udp_relay_socket_test.cpp
)
target_link_libraries(semilive_relay_udp_socket_test PRIVATE
    SemiLive::RelayInfraNetwork
)
semilive_enable_warnings(semilive_relay_udp_socket_test)

add_test(
    NAME semilive_relay_udp_socket
    COMMAND semilive_relay_udp_socket_test
)
