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
