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

add_library(semilive_publisher_infra_ffmpeg STATIC
    src/ffmpeg_raii.cpp
    src/video_encoder/ffmpeg_h264_encoder.cpp
    src/video_encoder/ffmpeg_h264_encoder_backend.cpp
    src/video_encoder/sws_frame_converter.cpp
)

add_library(SemiLive::PublisherInfraFfmpeg ALIAS semilive_publisher_infra_ffmpeg)

target_include_directories(semilive_publisher_infra_ffmpeg
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(semilive_publisher_infra_ffmpeg PUBLIC cxx_std_23)
target_link_libraries(semilive_publisher_infra_ffmpeg
    PUBLIC
        SemiLive::PublisherContracts
        SemiLive::PublisherModel
    PRIVATE
        PkgConfig::SEMILIVE_FFMPEG_AVCODEC
        PkgConfig::SEMILIVE_FFMPEG_SWSCALE
)
semilive_enable_warnings(semilive_publisher_infra_ffmpeg)
