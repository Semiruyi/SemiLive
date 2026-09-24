add_executable(semilive_receiver_rtp_parser_test
    rtp_parser_test.cpp
)
target_link_libraries(semilive_receiver_rtp_parser_test PRIVATE
    SemiLive::ReceiverDomain
)
semilive_enable_warnings(semilive_receiver_rtp_parser_test)

add_test(
    NAME semilive_receiver_rtp_parser
    COMMAND semilive_receiver_rtp_parser_test
)

add_executable(semilive_receiver_rtp_session_filter_test
    rtp_session_filter_test.cpp
)
target_link_libraries(semilive_receiver_rtp_session_filter_test PRIVATE
    SemiLive::ReceiverDomain
)
semilive_enable_warnings(semilive_receiver_rtp_session_filter_test)

add_test(
    NAME semilive_receiver_rtp_session_filter
    COMMAND semilive_receiver_rtp_session_filter_test
)

add_executable(semilive_receiver_rtp_reorder_buffer_test
    rtp_reorder_buffer_test.cpp
)
target_link_libraries(semilive_receiver_rtp_reorder_buffer_test PRIVATE
    SemiLive::ReceiverDomain
)
semilive_enable_warnings(semilive_receiver_rtp_reorder_buffer_test)

add_test(
    NAME semilive_receiver_rtp_reorder_buffer
    COMMAND semilive_receiver_rtp_reorder_buffer_test
)

add_executable(semilive_receiver_rtp_missing_tracker_test
    rtp_missing_tracker_test.cpp
)
target_link_libraries(semilive_receiver_rtp_missing_tracker_test PRIVATE
    SemiLive::ReceiverDomain
)
semilive_enable_warnings(semilive_receiver_rtp_missing_tracker_test)

add_test(
    NAME semilive_receiver_rtp_missing_tracker
    COMMAND semilive_receiver_rtp_missing_tracker_test
)

add_executable(semilive_receiver_rtp_timestamp_mapper_test
    rtp_timestamp_mapper_test.cpp
)
target_link_libraries(
    semilive_receiver_rtp_timestamp_mapper_test
    PRIVATE
        SemiLive::ReceiverDomain
)
semilive_enable_warnings(semilive_receiver_rtp_timestamp_mapper_test)

add_test(
    NAME semilive_receiver_rtp_timestamp_mapper
    COMMAND semilive_receiver_rtp_timestamp_mapper_test
)

add_executable(semilive_receiver_rtp_reception_statistics_test
    rtp_reception_statistics_test.cpp
)
target_link_libraries(
    semilive_receiver_rtp_reception_statistics_test
    PRIVATE
        SemiLive::ReceiverDomain
)
semilive_enable_warnings(semilive_receiver_rtp_reception_statistics_test)

add_test(
    NAME semilive_receiver_rtp_reception_statistics
    COMMAND semilive_receiver_rtp_reception_statistics_test
)
