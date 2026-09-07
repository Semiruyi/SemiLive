add_executable(semilive_publisher_resources_test
    publisher_resources_test.cpp
)
target_link_libraries(semilive_publisher_resources_test PRIVATE
    SemiLive::PublisherDomain
    semilive_publisher_test_support
)
semilive_enable_warnings(semilive_publisher_resources_test)

add_test(NAME semilive_publisher_resources COMMAND semilive_publisher_resources_test)
