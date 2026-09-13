add_library(semilive_receiver_infra_semiplayer INTERFACE)

add_library(SemiLive::ReceiverInfraSemiPlayer ALIAS
    semilive_receiver_infra_semiplayer
)

target_compile_features(semilive_receiver_infra_semiplayer INTERFACE
    cxx_std_23
)
target_link_libraries(semilive_receiver_infra_semiplayer INTERFACE
    SemiLive::ReceiverContracts
    SemiLive::ReceiverModel
)
