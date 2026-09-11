add_executable(semilive_publisher_video_encoding_pipeline_test
    video_encoding_pipeline_test.cpp
)
target_link_libraries(semilive_publisher_video_encoding_pipeline_test PRIVATE
    SemiLive::PublisherDomain
    SemiLive::PublisherInfraCapture
    SemiLive::PublisherInfraFfmpeg
    SemiLive::PublisherInfraNotifier
)
semilive_enable_warnings(semilive_publisher_video_encoding_pipeline_test)

add_test(
    NAME semilive_publisher_video_encoding_pipeline
    COMMAND semilive_publisher_video_encoding_pipeline_test
)

add_executable(semilive_publisher_video_file_output_pipeline_test
    video_file_output_pipeline_test.cpp
)
target_link_libraries(semilive_publisher_video_file_output_pipeline_test PRIVATE
    SemiLive::PublisherApplication
    SemiLive::PublisherDomain
    SemiLive::PublisherInfraCapture
    SemiLive::PublisherInfraFfmpeg
    SemiLive::PublisherInfraNotifier
    SemiLive::PublisherInfraOutput
)
semilive_enable_warnings(semilive_publisher_video_file_output_pipeline_test)

add_test(
    NAME semilive_publisher_video_file_output_pipeline
    COMMAND semilive_publisher_video_file_output_pipeline_test
)
