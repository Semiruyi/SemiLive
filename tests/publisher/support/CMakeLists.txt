add_library(semilive_publisher_test_support STATIC
    capture/scripted_desktop_capture_backend.cpp
    encoder/scripted_video_encoder_backend.cpp
    notifier/synchronous_notifier.cpp
)
target_include_directories(semilive_publisher_test_support PUBLIC
    ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(semilive_publisher_test_support PUBLIC
    SemiLive::PublisherContracts
)
semilive_enable_warnings(semilive_publisher_test_support)
