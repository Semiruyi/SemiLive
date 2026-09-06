find_package(Threads REQUIRED)

target_link_libraries(semilive_publisher_core PRIVATE
    Threads::Threads
)

add_subdirectory(resource)
add_subdirectory(timing)
add_subdirectory(worker)
