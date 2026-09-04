add_executable(semilive_publisher_video_capture_worker_test
    video_capture_worker_test.cpp
)
target_link_libraries(semilive_publisher_video_capture_worker_test PRIVATE
    semilive_publisher_core
)
semilive_enable_warnings(semilive_publisher_video_capture_worker_test)

add_test(
    NAME semilive_publisher_video_capture_worker
    COMMAND semilive_publisher_video_capture_worker_test
)
