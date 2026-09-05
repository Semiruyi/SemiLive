find_package(PkgConfig REQUIRED)

pkg_check_modules(SEMILIVE_FFMPEG_SWSCALE REQUIRED
    IMPORTED_TARGET GLOBAL
    libavutil
    libswscale
)

target_sources(semilive_publisher_core PRIVATE
    ffmpeg_raii.cpp
)

add_subdirectory(video_encoder)
