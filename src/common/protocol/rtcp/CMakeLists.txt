add_library(semilive_common_rtcp STATIC
    src/ntp_time.cpp
    src/rtcp_packet.cpp
)

add_library(SemiLive::CommonRtcp ALIAS semilive_common_rtcp)

target_include_directories(semilive_common_rtcp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_common_rtcp PUBLIC cxx_std_23)
semilive_enable_warnings(semilive_common_rtcp)
