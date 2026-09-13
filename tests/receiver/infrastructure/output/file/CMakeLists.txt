add_executable(semilive_receiver_h264_file_output_test
    h264_file_output_backend_test.cpp
)
target_link_libraries(
    semilive_receiver_h264_file_output_test
    PRIVATE
        SemiLive::ReceiverInfraFileOutput
)
semilive_enable_warnings(semilive_receiver_h264_file_output_test)

add_test(
    NAME semilive_receiver_h264_file_output
    COMMAND semilive_receiver_h264_file_output_test
)
