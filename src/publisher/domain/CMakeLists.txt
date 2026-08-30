find_package(Threads REQUIRED)

add_library(semilive_publisher_domain STATIC
    resource/captured_frame_store/captured_frame_store.cpp
    resource/encoded_access_unit_queue/encoded_access_unit_queue.cpp
)
target_include_directories(semilive_publisher_domain PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)
target_compile_features(semilive_publisher_domain PUBLIC cxx_std_23)
target_link_libraries(semilive_publisher_domain PUBLIC
    Threads::Threads
    semilive_publisher_notifier
)
semilive_enable_warnings(semilive_publisher_domain)
