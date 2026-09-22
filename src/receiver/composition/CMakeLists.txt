add_library(semilive_receiver_composition STATIC
    src/receiver_composition.cpp
)

add_library(SemiLive::ReceiverComposition ALIAS
    semilive_receiver_composition
)

target_include_directories(semilive_receiver_composition PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_receiver_composition PUBLIC cxx_std_23)
target_link_libraries(semilive_receiver_composition
    PUBLIC
        SemiLive::ReceiverApplication
    PRIVATE
        SemiLive::ReceiverDomain
        SemiLive::ReceiverInfrastructure
        SemiLive::CommonRtcpUdp
)
semilive_enable_warnings(semilive_receiver_composition)
