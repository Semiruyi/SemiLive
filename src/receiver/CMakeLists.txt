add_library(semilive_receiver_core INTERFACE)

add_library(SemiLive::ReceiverCore ALIAS semilive_receiver_core)

add_subdirectory(model)
add_subdirectory(contracts)
add_subdirectory(domain)
add_subdirectory(application)
add_subdirectory(infrastructure)
add_subdirectory(composition)
add_subdirectory(reporting)

target_compile_features(semilive_receiver_core INTERFACE cxx_std_23)
target_link_libraries(semilive_receiver_core INTERFACE
    SemiLive::ReceiverComposition
    SemiLive::ReceiverReporting
)

semilive_add_app(semilive_receiver main.cpp)
target_link_libraries(semilive_receiver PRIVATE
    SemiLive::ReceiverCore
    SemiLive::CommonLog
)
