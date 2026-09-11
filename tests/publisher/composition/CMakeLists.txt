add_executable(semilive_publisher_composition_test
    publisher_composition_test.cpp
)
target_link_libraries(semilive_publisher_composition_test PRIVATE
    SemiLive::PublisherComposition
)
semilive_enable_warnings(semilive_publisher_composition_test)

add_test(
    NAME semilive_publisher_composition
    COMMAND semilive_publisher_composition_test
)
