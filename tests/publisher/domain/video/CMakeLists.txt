add_executable(semilive_publisher_video_placement_test
    video_placement_test.cpp
)
target_link_libraries(semilive_publisher_video_placement_test PRIVATE
    semilive_publisher_core
)
semilive_enable_warnings(semilive_publisher_video_placement_test)

add_test(
    NAME semilive_publisher_video_placement
    COMMAND semilive_publisher_video_placement_test
)
