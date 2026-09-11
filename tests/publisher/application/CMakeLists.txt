add_executable(semilive_publisher_controller_test
    publisher_controller_test.cpp
)
target_link_libraries(semilive_publisher_controller_test PRIVATE
    SemiLive::PublisherApplication
    semilive_publisher_test_support
)
semilive_enable_warnings(semilive_publisher_controller_test)

add_test(
    NAME semilive_publisher_controller
    COMMAND semilive_publisher_controller_test
)
