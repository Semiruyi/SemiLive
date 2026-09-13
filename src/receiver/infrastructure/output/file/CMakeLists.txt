add_library(semilive_receiver_infra_file_output STATIC
    src/h264_file_output_backend.cpp
)

add_library(SemiLive::ReceiverInfraFileOutput ALIAS
    semilive_receiver_infra_file_output
)

target_include_directories(semilive_receiver_infra_file_output PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_receiver_infra_file_output PUBLIC
    cxx_std_23
)
target_link_libraries(semilive_receiver_infra_file_output PUBLIC
    SemiLive::ReceiverContracts
    SemiLive::ReceiverModel
)
semilive_enable_warnings(semilive_receiver_infra_file_output)
