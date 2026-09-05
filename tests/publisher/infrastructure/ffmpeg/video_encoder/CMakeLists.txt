add_executable(semilive_publisher_sws_frame_converter_test
    sws_frame_converter_test.cpp
)
target_link_libraries(semilive_publisher_sws_frame_converter_test PRIVATE
    semilive_publisher_core
    PkgConfig::SEMILIVE_FFMPEG_SWSCALE
)
semilive_enable_warnings(semilive_publisher_sws_frame_converter_test)

add_test(
    NAME semilive_publisher_sws_frame_converter
    COMMAND semilive_publisher_sws_frame_converter_test
)
