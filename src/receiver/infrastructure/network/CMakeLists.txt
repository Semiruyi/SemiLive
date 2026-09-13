add_library(semilive_receiver_infra_network INTERFACE)

add_library(SemiLive::ReceiverInfraNetwork ALIAS
    semilive_receiver_infra_network
)

target_compile_features(semilive_receiver_infra_network INTERFACE cxx_std_23)
target_link_libraries(semilive_receiver_infra_network INTERFACE
    SemiLive::ReceiverContracts
    SemiLive::ReceiverModel
)
