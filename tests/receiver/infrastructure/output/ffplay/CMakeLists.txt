add_executable(semilive_receiver_ffplay_sink_helper
    ffplay_sink_helper.cpp
)
target_compile_features(semilive_receiver_ffplay_sink_helper PRIVATE
    cxx_std_23
)
semilive_enable_warnings(semilive_receiver_ffplay_sink_helper)

add_executable(semilive_receiver_ffplay_output_test
    ffplay_video_output_backend_test.cpp
)
target_link_libraries(semilive_receiver_ffplay_output_test PRIVATE
    SemiLive::ReceiverInfraFfplayOutput
)
target_compile_definitions(semilive_receiver_ffplay_output_test PRIVATE
    SEMILIVE_FFPLAY_TEST_HELPER="$<TARGET_FILE:semilive_receiver_ffplay_sink_helper>"
)
add_dependencies(semilive_receiver_ffplay_output_test
    semilive_receiver_ffplay_sink_helper
)
semilive_enable_warnings(semilive_receiver_ffplay_output_test)

add_test(
    NAME semilive_receiver_ffplay_output
    COMMAND semilive_receiver_ffplay_output_test
)
