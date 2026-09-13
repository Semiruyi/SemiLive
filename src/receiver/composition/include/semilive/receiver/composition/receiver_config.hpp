#pragma once

#include <semilive/receiver/contracts/network/datagram_source_backend.hpp>
#include <semilive/receiver/domain/pipeline/h264_rtp_receive_pipeline.hpp>

#include <chrono>
#include <filesystem>

namespace semilive::receiver::composition {

struct ReceiverConfig {
    contracts::network::DatagramSourceConfig input;
    domain::H264RtpReceivePipelineConfig pipeline;
    std::filesystem::path h264_output_path{"semilive-received.h264"};
    std::chrono::milliseconds receive_poll_interval{10};
};

}  // namespace semilive::receiver::composition
