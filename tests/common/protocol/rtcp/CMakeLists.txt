add_executable(semilive_common_rtcp_test
    rtcp_packet_test.cpp
)
target_compile_features(semilive_common_rtcp_test PRIVATE cxx_std_23)
target_link_libraries(semilive_common_rtcp_test PRIVATE
    SemiLive::CommonRtcp
)
semilive_enable_warnings(semilive_common_rtcp_test)

add_test(NAME semilive_common_rtcp COMMAND semilive_common_rtcp_test)
