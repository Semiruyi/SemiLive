add_subdirectory(infrastructure)
add_subdirectory(domain)

semilive_add_app(semilive_publisher main.cpp)
target_link_libraries(semilive_publisher PRIVATE
    semilive_common_log
    semilive_publisher_domain
)
