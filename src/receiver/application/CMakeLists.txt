add_library(semilive_receiver_application STATIC
    src/default_receiver_controller.cpp
)

add_library(SemiLive::ReceiverApplication ALIAS
    semilive_receiver_application
)

target_include_directories(semilive_receiver_application PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_receiver_application PUBLIC cxx_std_23)
target_link_libraries(semilive_receiver_application PUBLIC
    SemiLive::ReceiverDomain
)
semilive_enable_warnings(semilive_receiver_application)
