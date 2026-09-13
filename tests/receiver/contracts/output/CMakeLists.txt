add_executable(semilive_receiver_live_video_output_contract_test
    live_video_output_backend_test.cpp
)
target_link_libraries(
    semilive_receiver_live_video_output_contract_test
    PRIVATE
        SemiLive::ReceiverContracts
)
semilive_enable_warnings(semilive_receiver_live_video_output_contract_test)

add_test(
    NAME semilive_receiver_live_video_output_contract
    COMMAND semilive_receiver_live_video_output_contract_test
)
