add_executable(semilive_common_log_test
    log_test.cpp
)
target_compile_features(semilive_common_log_test PRIVATE cxx_std_23)
target_link_libraries(semilive_common_log_test PRIVATE
    SemiLive::CommonLog
)
semilive_enable_warnings(semilive_common_log_test)

add_test(NAME semilive_common_log COMMAND semilive_common_log_test)
