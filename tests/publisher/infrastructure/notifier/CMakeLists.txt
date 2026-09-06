add_executable(semilive_publisher_default_notifier_test
    default_notifier_test.cpp
)
target_link_libraries(semilive_publisher_default_notifier_test PRIVATE
    semilive_publisher_core
)
semilive_enable_warnings(semilive_publisher_default_notifier_test)

add_test(
    NAME semilive_publisher_default_notifier
    COMMAND semilive_publisher_default_notifier_test
)
