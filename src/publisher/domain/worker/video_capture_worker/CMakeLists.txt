target_sources(semilive_publisher_core PRIVATE
    default_video_capture_worker.cpp
)

if(WIN32)
    target_link_libraries(semilive_publisher_core PRIVATE ole32)
endif()
