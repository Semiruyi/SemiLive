add_library(semilive_receiver_infra_network STATIC
    src/udp_datagram_source_backend.cpp
)

add_library(SemiLive::ReceiverInfraNetwork ALIAS
    semilive_receiver_infra_network
)

target_include_directories(semilive_receiver_infra_network PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_receiver_infra_network PUBLIC cxx_std_23)
target_link_libraries(semilive_receiver_infra_network PUBLIC
    SemiLive::ReceiverContracts
    SemiLive::ReceiverModel
)
if(WIN32)
    target_link_libraries(semilive_receiver_infra_network PRIVATE
        ws2_32
    )
endif()
semilive_enable_warnings(semilive_receiver_infra_network)
