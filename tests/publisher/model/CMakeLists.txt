add_executable(semilive_publisher_media_clock_test
    media_clock_test.cpp
)
target_link_libraries(semilive_publisher_media_clock_test PRIVATE
    SemiLive::PublisherModel
)
semilive_enable_warnings(semilive_publisher_media_clock_test)

add_test(
    NAME semilive_publisher_media_clock
    COMMAND semilive_publisher_media_clock_test
)

add_subdirectory(video)
