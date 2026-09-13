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
