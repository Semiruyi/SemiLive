add_executable(semilive_publisher_video_output_contract_test
    video_access_unit_output_backend_test.cpp
)
target_link_libraries(semilive_publisher_video_output_contract_test PRIVATE
    semilive_publisher_test_support
    SemiLive::PublisherContracts
)
semilive_enable_warnings(semilive_publisher_video_output_contract_test)

add_test(
    NAME semilive_publisher_video_output_contract
    COMMAND semilive_publisher_video_output_contract_test
)
