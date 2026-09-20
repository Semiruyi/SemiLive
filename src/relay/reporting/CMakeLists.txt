add_library(semilive_relay_reporting STATIC
    src/relay_session_report.cpp
)

add_library(SemiLive::RelayReporting ALIAS
    semilive_relay_reporting
)

target_include_directories(semilive_relay_reporting PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_relay_reporting PUBLIC cxx_std_23)
target_link_libraries(semilive_relay_reporting PUBLIC
    SemiLive::RelayApplication
)
semilive_enable_warnings(semilive_relay_reporting)
