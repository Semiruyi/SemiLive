add_executable(semilive_receiver_lifecycle_smoke_test
    receiver_lifecycle_smoke_test.cpp
)
target_link_libraries(semilive_receiver_lifecycle_smoke_test PRIVATE
    SemiLive::ReceiverComposition
)
semilive_enable_warnings(semilive_receiver_lifecycle_smoke_test)

add_test(
    NAME semilive_receiver_lifecycle_smoke
    COMMAND semilive_receiver_lifecycle_smoke_test
)

add_subdirectory(domain/rtp)
add_subdirectory(domain/h264)
add_subdirectory(domain/pipeline)
