add_executable(semilive_publisher_rtp_timestamp_test
    rtp_timestamp_test.cpp
)
target_link_libraries(semilive_publisher_rtp_timestamp_test PRIVATE
    semilive_publisher_core
)
semilive_enable_warnings(semilive_publisher_rtp_timestamp_test)

add_test(
    NAME semilive_publisher_rtp_timestamp
    COMMAND semilive_publisher_rtp_timestamp_test
)
