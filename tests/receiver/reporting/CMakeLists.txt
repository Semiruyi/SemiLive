add_executable(semilive_receiver_session_report_test
    receiver_session_report_test.cpp
)
target_link_libraries(semilive_receiver_session_report_test PRIVATE
    SemiLive::ReceiverReporting
)
semilive_enable_warnings(semilive_receiver_session_report_test)

add_test(
    NAME semilive_receiver_session_report
    COMMAND semilive_receiver_session_report_test
)
