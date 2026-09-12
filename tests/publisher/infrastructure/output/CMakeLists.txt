add_executable(semilive_publisher_h264_file_output_test
    h264_file_output_backend_test.cpp
)
target_link_libraries(semilive_publisher_h264_file_output_test PRIVATE
    SemiLive::PublisherInfraOutput
)
semilive_enable_warnings(semilive_publisher_h264_file_output_test)

add_test(
    NAME semilive_publisher_h264_file_output
    COMMAND semilive_publisher_h264_file_output_test
)

add_executable(semilive_publisher_h264_rtp_packetizer_test
    h264_rtp_packetizer_test.cpp
)
target_include_directories(semilive_publisher_h264_rtp_packetizer_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/publisher/infrastructure/output/src
)
target_link_libraries(semilive_publisher_h264_rtp_packetizer_test PRIVATE
    SemiLive::PublisherInfraOutput
)
semilive_enable_warnings(semilive_publisher_h264_rtp_packetizer_test)

add_test(
    NAME semilive_publisher_h264_rtp_packetizer
    COMMAND semilive_publisher_h264_rtp_packetizer_test
)

add_executable(semilive_publisher_udp_socket_test
    udp_socket_test.cpp
)
target_include_directories(semilive_publisher_udp_socket_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/publisher/infrastructure/output/src
)
target_link_libraries(semilive_publisher_udp_socket_test PRIVATE
    SemiLive::PublisherInfraOutput
)
if(WIN32)
    target_link_libraries(semilive_publisher_udp_socket_test PRIVATE
        ws2_32
    )
endif()
semilive_enable_warnings(semilive_publisher_udp_socket_test)

add_test(
    NAME semilive_publisher_udp_socket
    COMMAND semilive_publisher_udp_socket_test
)

add_executable(semilive_publisher_rtp_udp_video_output_test
    rtp_udp_video_output_backend_test.cpp
)
target_include_directories(
    semilive_publisher_rtp_udp_video_output_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/publisher/infrastructure/output/src
)
target_link_libraries(
    semilive_publisher_rtp_udp_video_output_test PRIVATE
    SemiLive::PublisherInfraOutput
)
if(WIN32)
    target_link_libraries(
        semilive_publisher_rtp_udp_video_output_test PRIVATE
        ws2_32
    )
endif()
semilive_enable_warnings(semilive_publisher_rtp_udp_video_output_test)

add_test(
    NAME semilive_publisher_rtp_udp_video_output
    COMMAND semilive_publisher_rtp_udp_video_output_test
)
