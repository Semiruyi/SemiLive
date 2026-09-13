add_executable(semilive_receiver_video_receive_worker_test
    default_video_receive_worker_test.cpp
)
target_link_libraries(
    semilive_receiver_video_receive_worker_test
    PRIVATE
        SemiLive::ReceiverDomain
)
semilive_enable_warnings(semilive_receiver_video_receive_worker_test)

add_test(
    NAME semilive_receiver_video_receive_worker
    COMMAND semilive_receiver_video_receive_worker_test
)
