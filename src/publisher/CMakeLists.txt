add_library(semilive_publisher_core INTERFACE)

add_library(SemiLive::PublisherCore ALIAS semilive_publisher_core)

add_subdirectory(model)
add_subdirectory(contracts)
add_subdirectory(domain)
add_subdirectory(application)
add_subdirectory(infrastructure)
add_subdirectory(composition)

target_compile_features(semilive_publisher_core INTERFACE cxx_std_23)
target_link_libraries(semilive_publisher_core INTERFACE
    SemiLive::PublisherComposition
)
semilive_add_app(semilive_publisher main.cpp)
target_link_libraries(semilive_publisher PRIVATE
    SemiLive::PublisherCore
    SemiLive::CommonLog
)
