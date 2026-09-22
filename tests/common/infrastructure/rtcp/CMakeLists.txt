add_executable(semilive_common_rtcp_udp_test
    udp_rtcp_transport_test.cpp
)
target_compile_features(semilive_common_rtcp_udp_test PRIVATE cxx_std_23)
target_link_libraries(semilive_common_rtcp_udp_test PRIVATE
    SemiLive::CommonRtcpUdp
)
semilive_enable_warnings(semilive_common_rtcp_udp_test)

add_test(NAME semilive_common_rtcp_udp
         COMMAND semilive_common_rtcp_udp_test)
