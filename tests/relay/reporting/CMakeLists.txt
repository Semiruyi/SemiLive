add_executable(semilive_relay_session_report_test
    relay_session_report_test.cpp
)
target_link_libraries(semilive_relay_session_report_test PRIVATE
    SemiLive::RelayReporting
)
semilive_enable_warnings(semilive_relay_session_report_test)

add_test(
    NAME semilive_relay_session_report
    COMMAND semilive_relay_session_report_test
)
