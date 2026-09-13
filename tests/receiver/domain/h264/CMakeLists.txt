add_executable(semilive_receiver_h264_rtp_depacketizer_test
    h264_rtp_depacketizer_test.cpp
)
target_link_libraries(semilive_receiver_h264_rtp_depacketizer_test PRIVATE
    SemiLive::ReceiverDomain
)
semilive_enable_warnings(semilive_receiver_h264_rtp_depacketizer_test)

add_test(
    NAME semilive_receiver_h264_rtp_depacketizer
    COMMAND semilive_receiver_h264_rtp_depacketizer_test
)

add_executable(semilive_receiver_h264_access_unit_assembler_test
    h264_access_unit_assembler_test.cpp
)
target_link_libraries(
    semilive_receiver_h264_access_unit_assembler_test
    PRIVATE
        SemiLive::ReceiverDomain
)
semilive_enable_warnings(
    semilive_receiver_h264_access_unit_assembler_test
)

add_test(
    NAME semilive_receiver_h264_access_unit_assembler
    COMMAND semilive_receiver_h264_access_unit_assembler_test
)
