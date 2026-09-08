add_executable(semilive_publisher_sws_frame_converter_test
    sws_frame_converter_test.cpp
)
target_link_libraries(semilive_publisher_sws_frame_converter_test PRIVATE
    SemiLive::PublisherInfraFfmpeg
    PkgConfig::SEMILIVE_FFMPEG_SWSCALE
)
target_include_directories(semilive_publisher_sws_frame_converter_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/publisher/infrastructure/ffmpeg/src
)
semilive_enable_warnings(semilive_publisher_sws_frame_converter_test)

add_test(
    NAME semilive_publisher_sws_frame_converter
    COMMAND semilive_publisher_sws_frame_converter_test
)

add_executable(semilive_publisher_ffmpeg_h264_encoder_test
    ffmpeg_h264_encoder_test.cpp
)
target_link_libraries(semilive_publisher_ffmpeg_h264_encoder_test PRIVATE
    SemiLive::PublisherInfraFfmpeg
    PkgConfig::SEMILIVE_FFMPEG_AVCODEC
)
target_include_directories(semilive_publisher_ffmpeg_h264_encoder_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/publisher/infrastructure/ffmpeg/src
)
semilive_enable_warnings(semilive_publisher_ffmpeg_h264_encoder_test)

add_test(
    NAME semilive_publisher_ffmpeg_h264_encoder
    COMMAND semilive_publisher_ffmpeg_h264_encoder_test
)

add_executable(semilive_publisher_ffmpeg_h264_encoder_backend_test
    ffmpeg_h264_encoder_backend_test.cpp
)
target_link_libraries(semilive_publisher_ffmpeg_h264_encoder_backend_test PRIVATE
    SemiLive::PublisherInfraFfmpeg
)
semilive_enable_warnings(semilive_publisher_ffmpeg_h264_encoder_backend_test)

add_test(
    NAME semilive_publisher_ffmpeg_h264_encoder_backend
    COMMAND semilive_publisher_ffmpeg_h264_encoder_backend_test
)
