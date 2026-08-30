add_library(semilive_publisher_core STATIC)
target_include_directories(semilive_publisher_core PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)
target_compile_features(semilive_publisher_core PUBLIC cxx_std_23)
semilive_enable_warnings(semilive_publisher_core)
semilive_target_add_logging(semilive_publisher_core)

add_subdirectory(infrastructure)
add_subdirectory(domain)

semilive_add_app(semilive_publisher main.cpp)
target_link_libraries(semilive_publisher PRIVATE
    semilive_publisher_core
)
