add_subdirectory(capture)
add_subdirectory(ffmpeg)
add_subdirectory(notifier)
add_subdirectory(output)

add_library(semilive_publisher_infrastructure INTERFACE)

add_library(SemiLive::PublisherInfrastructure ALIAS semilive_publisher_infrastructure)

target_link_libraries(semilive_publisher_infrastructure INTERFACE
    SemiLive::PublisherInfraCapture
    SemiLive::PublisherInfraFfmpeg
    SemiLive::PublisherInfraNotifier
    SemiLive::PublisherInfraOutput
)
