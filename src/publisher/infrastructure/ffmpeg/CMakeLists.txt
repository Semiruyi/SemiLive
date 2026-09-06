find_package(PkgConfig REQUIRED)

pkg_check_modules(SEMILIVE_FFMPEG_SWSCALE REQUIRED
    IMPORTED_TARGET GLOBAL
    libavutil
    libswscale
)

pkg_check_modules(SEMILIVE_FFMPEG_AVCODEC REQUIRED
    IMPORTED_TARGET GLOBAL
    libavcodec
    libavutil
)

target_sources(semilive_publisher_core PRIVATE
    ffmpeg_raii.cpp
)

target_link_libraries(semilive_publisher_core PRIVATE
    PkgConfig::SEMILIVE_FFMPEG_AVCODEC
)

add_subdirectory(video_encoder)
