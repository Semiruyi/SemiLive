add_library(semilive_publisher_contracts INTERFACE)

add_library(SemiLive::PublisherContracts ALIAS semilive_publisher_contracts)

target_include_directories(semilive_publisher_contracts INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_publisher_contracts INTERFACE cxx_std_23)
target_link_libraries(semilive_publisher_contracts INTERFACE
    SemiLive::PublisherModel
)
