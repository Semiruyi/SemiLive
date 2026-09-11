find_package(Threads REQUIRED)

add_library(semilive_publisher_application STATIC
    src/publisher_controller/default_publisher_controller.cpp
)

add_library(SemiLive::PublisherApplication ALIAS semilive_publisher_application)

target_include_directories(semilive_publisher_application PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_publisher_application PUBLIC cxx_std_23)
target_link_libraries(semilive_publisher_application
    PUBLIC
        SemiLive::PublisherDomain
    PRIVATE
        Threads::Threads
)
semilive_enable_warnings(semilive_publisher_application)
