add_library(semilive_relay_application STATIC
    src/relay_session.cpp
)

add_library(SemiLive::RelayApplication ALIAS
    semilive_relay_application
)

target_include_directories(semilive_relay_application PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_relay_application PUBLIC cxx_std_23)
target_link_libraries(semilive_relay_application PUBLIC
    SemiLive::RelayDomain
    SemiLive::RelayInfraNetwork
)
semilive_enable_warnings(semilive_relay_application)
