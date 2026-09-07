add_library(semilive_publisher_model STATIC
    src/media_clock.cpp
    src/video/video_placement_calculator.cpp
)

add_library(SemiLive::PublisherModel ALIAS semilive_publisher_model)

target_include_directories(semilive_publisher_model PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_publisher_model PUBLIC cxx_std_23)
semilive_enable_warnings(semilive_publisher_model)
