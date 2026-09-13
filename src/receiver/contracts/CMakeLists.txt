add_library(semilive_receiver_contracts INTERFACE)

add_library(SemiLive::ReceiverContracts ALIAS semilive_receiver_contracts)

target_include_directories(semilive_receiver_contracts INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_receiver_contracts INTERFACE cxx_std_23)
target_link_libraries(semilive_receiver_contracts INTERFACE
    SemiLive::ReceiverModel
)
