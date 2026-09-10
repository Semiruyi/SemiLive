find_package(Threads REQUIRED)

add_library(semilive_publisher_domain STATIC
    src/resource/captured_video_frame_store/captured_video_frame_store.cpp
    src/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue.cpp
    src/timing/frame_scheduler.cpp
    src/timing/session_timeline.cpp
    src/worker/video_capture_worker/default_video_capture_worker.cpp
    src/worker/video_encoder_worker/default_video_encoder_worker.cpp
    src/worker/video_output_worker/default_video_output_worker.cpp
)

add_library(SemiLive::PublisherDomain ALIAS semilive_publisher_domain)

target_include_directories(semilive_publisher_domain PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_publisher_domain PUBLIC cxx_std_23)
target_link_libraries(semilive_publisher_domain
    PUBLIC
        SemiLive::PublisherContracts
        SemiLive::PublisherModel
    PRIVATE
        Threads::Threads
)
semilive_enable_warnings(semilive_publisher_domain)
