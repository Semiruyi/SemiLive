add_library(semilive_common_rtcp_udp STATIC
    src/udp_rtcp_transport.cpp
)

add_library(SemiLive::CommonRtcpUdp ALIAS semilive_common_rtcp_udp)

target_include_directories(semilive_common_rtcp_udp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_common_rtcp_udp PUBLIC cxx_std_23)
target_link_libraries(semilive_common_rtcp_udp PUBLIC
    SemiLive::CommonRtcp
)
if(WIN32)
    target_link_libraries(semilive_common_rtcp_udp PRIVATE ws2_32)
endif()
semilive_enable_warnings(semilive_common_rtcp_udp)
