add_executable(semilive_publisher_video_capture_worker_test
    video_capture_worker_test.cpp
)
target_link_libraries(semilive_publisher_video_capture_worker_test PRIVATE
    SemiLive::PublisherDomain
    semilive_publisher_test_support
)
semilive_enable_warnings(semilive_publisher_video_capture_worker_test)

add_test(
    NAME semilive_publisher_video_capture_worker
    COMMAND semilive_publisher_video_capture_worker_test
)

add_executable(semilive_publisher_video_encoder_worker_test
    video_encoder_worker_test.cpp
)
target_link_libraries(semilive_publisher_video_encoder_worker_test PRIVATE
    SemiLive::PublisherDomain
    semilive_publisher_test_support
)
semilive_enable_warnings(semilive_publisher_video_encoder_worker_test)

add_test(
    NAME semilive_publisher_video_encoder_worker
    COMMAND semilive_publisher_video_encoder_worker_test
)

add_executable(semilive_publisher_video_output_worker_test
    video_output_worker_test.cpp
)
target_link_libraries(semilive_publisher_video_output_worker_test PRIVATE
    SemiLive::PublisherDomain
    semilive_publisher_test_support
)
semilive_enable_warnings(semilive_publisher_video_output_worker_test)

add_test(
    NAME semilive_publisher_video_output_worker
    COMMAND semilive_publisher_video_output_worker_test
)
