add_executable(semilive_receiver_h264_rtp_receive_pipeline_test
    h264_rtp_receive_pipeline_test.cpp
)
target_link_libraries(
    semilive_receiver_h264_rtp_receive_pipeline_test
    PRIVATE
        SemiLive::ReceiverDomain
)
semilive_enable_warnings(
    semilive_receiver_h264_rtp_receive_pipeline_test
)

add_test(
    NAME semilive_receiver_h264_rtp_receive_pipeline
    COMMAND semilive_receiver_h264_rtp_receive_pipeline_test
)
