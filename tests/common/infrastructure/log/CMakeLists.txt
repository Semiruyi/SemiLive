add_executable(semilive_common_log_test
    log_test.cpp
)
target_link_libraries(semilive_common_log_test PRIVATE
    semilive_common_log
)
semilive_enable_warnings(semilive_common_log_test)

add_test(NAME semilive_common_log COMMAND semilive_common_log_test)
