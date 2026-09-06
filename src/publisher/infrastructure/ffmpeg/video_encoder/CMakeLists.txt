target_sources(semilive_publisher_core PRIVATE
    ffmpeg_h264_encoder.cpp
    sws_frame_converter.cpp
)

target_link_libraries(semilive_publisher_core PRIVATE
    PkgConfig::SEMILIVE_FFMPEG_SWSCALE
)
