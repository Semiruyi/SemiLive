find_package(Threads REQUIRED)

add_library(semilive_receiver_domain STATIC
    src/h264/h264_access_unit_assembler.cpp
    src/h264/h264_recovery_gate.cpp
    src/h264/h264_rtp_depacketizer.cpp
    src/rtp/rtp_parser.cpp
    src/rtp/rtp_reorder_buffer.cpp
    src/rtp/rtp_session_filter.cpp
    src/rtp/rtp_timestamp_mapper.cpp
    src/worker/default_video_receive_worker.cpp
)

add_library(SemiLive::ReceiverDomain ALIAS semilive_receiver_domain)

target_include_directories(semilive_receiver_domain PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_receiver_domain PUBLIC cxx_std_23)
target_link_libraries(semilive_receiver_domain
    PUBLIC
        SemiLive::ReceiverContracts
        SemiLive::ReceiverModel
    PRIVATE
        Threads::Threads
)
semilive_enable_warnings(semilive_receiver_domain)
