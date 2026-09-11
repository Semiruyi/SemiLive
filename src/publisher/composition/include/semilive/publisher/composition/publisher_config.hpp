#pragma once

#include <semilive/publisher/contracts/capture/desktop_capture_backend.hpp>
#include <semilive/publisher/contracts/encoder/video_encoder_backend.hpp>

#include <chrono>
#include <filesystem>

namespace semilive::publisher::composition {

struct PublisherVideoConfig {
    contracts::capture::DesktopCaptureConfig capture;
    std::chrono::milliseconds recovery_timeout{5000};
    contracts::encoder::VideoEncoderConfig encoder;
};

struct PublisherH264FileOutputConfig {
    std::filesystem::path path{"semilive.h264"};
};

struct PublisherConfig {
    PublisherVideoConfig video;
    PublisherH264FileOutputConfig output;
};

}  // namespace semilive::publisher::composition
