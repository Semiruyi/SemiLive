find_package(spdlog CONFIG REQUIRED)

add_library(semilive_common_log STATIC
    log.cpp
)
target_include_directories(semilive_common_log PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)
target_compile_features(semilive_common_log PUBLIC cxx_std_23)
target_link_libraries(semilive_common_log PUBLIC
    spdlog::spdlog
)
semilive_enable_warnings(semilive_common_log)
