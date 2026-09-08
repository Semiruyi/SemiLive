find_package(spdlog CONFIG REQUIRED)

add_library(semilive_common_log STATIC
    src/log.cpp
)

add_library(SemiLive::CommonLog ALIAS semilive_common_log)

target_include_directories(semilive_common_log PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_common_log PUBLIC cxx_std_23)
target_link_libraries(semilive_common_log PUBLIC
    spdlog::spdlog
)
semilive_enable_warnings(semilive_common_log)
