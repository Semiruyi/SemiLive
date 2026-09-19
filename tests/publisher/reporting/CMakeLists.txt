add_executable(semilive_publisher_session_report_test
    publisher_session_report_test.cpp
)
target_compile_features(semilive_publisher_session_report_test PRIVATE cxx_std_23)
target_link_libraries(semilive_publisher_session_report_test PRIVATE
    SemiLive::PublisherReporting
)
semilive_enable_warnings(semilive_publisher_session_report_test)

add_test(
    NAME semilive_publisher_session_report
    COMMAND semilive_publisher_session_report_test
)
