add_library(semilive_relay_infra_network STATIC
    src/udp_relay_socket.cpp
)

add_library(SemiLive::RelayInfraNetwork ALIAS
    semilive_relay_infra_network
)

target_include_directories(semilive_relay_infra_network PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_relay_infra_network PUBLIC cxx_std_23)
if(WIN32)
    target_link_libraries(semilive_relay_infra_network PRIVATE ws2_32)
endif()
semilive_enable_warnings(semilive_relay_infra_network)
