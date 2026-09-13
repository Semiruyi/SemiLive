add_library(semilive_receiver_model INTERFACE)

add_library(SemiLive::ReceiverModel ALIAS semilive_receiver_model)

target_include_directories(semilive_receiver_model INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_receiver_model INTERFACE cxx_std_23)
