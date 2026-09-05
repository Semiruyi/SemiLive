add_library(semilive_publisher_test_support STATIC
    encoder/scripted_video_encoder_backend.cpp
)
target_include_directories(semilive_publisher_test_support PUBLIC
    ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(semilive_publisher_test_support PUBLIC
    semilive_publisher_core
)
semilive_enable_warnings(semilive_publisher_test_support)
