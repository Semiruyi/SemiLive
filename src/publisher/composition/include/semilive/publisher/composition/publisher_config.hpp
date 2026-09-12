#pragma once

#include <semilive/publisher/contracts/capture/desktop_capture_backend.hpp>
#include <semilive/publisher/contracts/encoder/video_encoder_backend.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace semilive::publisher::composition {

struct PublisherVideoConfig {
    contracts::capture::DesktopCaptureConfig capture;
    std::chrono::milliseconds recovery_timeout{5000};
    contracts::encoder::VideoEncoderConfig encoder;
};

struct RtpUdpVideoOutputConfig {
    std::string destination_address;
    std::uint16_t destination_port = 0;
    std::uint8_t payload_type = 96;
    std::size_t max_datagram_bytes = 1200;
};

struct PublisherVideoOutputConfig {
    RtpUdpVideoOutputConfig rtp_udp;
};

struct PublisherConfig {
    PublisherVideoConfig video;
    PublisherVideoOutputConfig output;
};

}  // namespace semilive::publisher::composition
