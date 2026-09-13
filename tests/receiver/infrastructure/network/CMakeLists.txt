add_executable(semilive_receiver_udp_datagram_source_test
    udp_datagram_source_backend_test.cpp
)
target_link_libraries(
    semilive_receiver_udp_datagram_source_test
    PRIVATE
        SemiLive::ReceiverInfraNetwork
)
if(WIN32)
    target_link_libraries(
        semilive_receiver_udp_datagram_source_test
        PRIVATE
            ws2_32
    )
endif()
semilive_enable_warnings(semilive_receiver_udp_datagram_source_test)

add_test(
    NAME semilive_receiver_udp_datagram_source
    COMMAND semilive_receiver_udp_datagram_source_test
)
