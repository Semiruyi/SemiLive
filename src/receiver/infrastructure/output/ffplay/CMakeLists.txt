add_library(semilive_receiver_infra_ffplay_output STATIC
    src/ffplay_video_output_backend.cpp
)

add_library(SemiLive::ReceiverInfraFfplayOutput ALIAS
    semilive_receiver_infra_ffplay_output
)

target_include_directories(semilive_receiver_infra_ffplay_output PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_receiver_infra_ffplay_output PUBLIC
    cxx_std_23
)
target_link_libraries(semilive_receiver_infra_ffplay_output PUBLIC
    SemiLive::ReceiverContracts
    SemiLive::ReceiverModel
)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(semilive_receiver_infra_ffplay_output PRIVATE
        Threads::Threads
    )
endif()
semilive_enable_warnings(semilive_receiver_infra_ffplay_output)
