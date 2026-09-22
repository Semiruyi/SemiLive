#pragma once

#include <semilive/receiver/contracts/network/datagram_source_backend.hpp>
#include <semilive/receiver/domain/pipeline/h264_rtp_receive_pipeline.hpp>
#include <semilive/common/rtcp/rtcp_transport.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace semilive::receiver::composition {

enum class ReceiverVideoOutputMode : std::uint8_t {
    File,
    Ffplay,
};

struct ReceiverFfplayOutputConfig {
    std::filesystem::path executable_path{"ffplay"};
    std::size_t maximum_buffered_access_units = 32;
    std::size_t maximum_buffered_bytes = 4U * 1024U * 1024U;
};

struct ReceiverRtcpConfig {
    common::rtcp::TransportConfig transport;
    std::chrono::milliseconds report_interval{1000};
    std::chrono::milliseconds receive_poll_interval{20};
};

struct ReceiverConfig {
    contracts::network::DatagramSourceConfig input;
    domain::H264RtpReceivePipelineConfig pipeline;
    ReceiverVideoOutputMode output_mode = ReceiverVideoOutputMode::File;
    std::filesystem::path h264_output_path{"semilive-received.h264"};
    ReceiverFfplayOutputConfig ffplay;
    std::chrono::milliseconds receive_poll_interval{10};
    std::chrono::milliseconds output_stall_threshold{100};
    std::optional<ReceiverRtcpConfig> rtcp;
};

}  // namespace semilive::receiver::composition
