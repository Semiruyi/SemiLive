add_executable(semilive_common_log_test
    log_test.cpp
)
target_include_directories(semilive_common_log_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_compile_features(semilive_common_log_test PRIVATE cxx_std_23)
semilive_target_add_logging(semilive_common_log_test)
semilive_enable_warnings(semilive_common_log_test)

add_test(NAME semilive_common_log COMMAND semilive_common_log_test)
