add_library(semilive_publisher_infra_output STATIC
    src/h264_file_output_backend.cpp
)

add_library(SemiLive::PublisherInfraOutput ALIAS semilive_publisher_infra_output)

target_include_directories(semilive_publisher_infra_output PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_publisher_infra_output PUBLIC cxx_std_23)
target_link_libraries(semilive_publisher_infra_output PUBLIC
    SemiLive::PublisherContracts
)
semilive_enable_warnings(semilive_publisher_infra_output)
