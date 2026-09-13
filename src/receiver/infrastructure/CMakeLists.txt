add_subdirectory(network)
add_subdirectory(output/file)
add_subdirectory(output/semiplayer)

add_library(semilive_receiver_infrastructure INTERFACE)

add_library(SemiLive::ReceiverInfrastructure ALIAS
    semilive_receiver_infrastructure
)

target_link_libraries(semilive_receiver_infrastructure INTERFACE
    SemiLive::ReceiverInfraNetwork
    SemiLive::ReceiverInfraFileOutput
    SemiLive::ReceiverInfraSemiPlayer
)
