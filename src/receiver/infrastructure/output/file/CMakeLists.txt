add_library(semilive_receiver_infra_file_output INTERFACE)

add_library(SemiLive::ReceiverInfraFileOutput ALIAS
    semilive_receiver_infra_file_output
)

target_compile_features(semilive_receiver_infra_file_output INTERFACE
    cxx_std_23
)
target_link_libraries(semilive_receiver_infra_file_output INTERFACE
    SemiLive::ReceiverContracts
    SemiLive::ReceiverModel
)
