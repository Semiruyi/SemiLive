#include <semilive/publisher/domain/resource/captured_video_frame_store/captured_video_frame_store.hpp>
#include <semilive/publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue.hpp>
#include <semilive/publisher/domain/worker/video_capture_worker/default_video_capture_worker.hpp>
#include <semilive/publisher/domain/worker/video_encoder_worker/default_video_encoder_worker.hpp>
#include <semilive/publisher/infrastructure/capture/synthetic_desktop_capture_backend.hpp>
#include <semilive/publisher/infrastructure/ffmpeg/video_encoder/ffmpeg_h264_encoder_backend.hpp>
#include <semilive/publisher/infrastructure/notifier/default_notifier.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace capture = semilive::publisher::contracts::capture;
namespace domain = semilive::publisher::domain;
namespace ffmpeg = semilive::publisher::infra::ffmpeg;
namespace infra = semilive::publisher::infra;
namespace model = semilive::publisher::model;
namespace synthetic = semilive::publisher::infra::capture;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename Predicate>
bool wait_until(Predicate predicate,
                const std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

capture::DesktopImage patterned_image(const std::uint8_t phase) {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    constexpr std::uint32_t stride = width * 4U;

    std::vector<std::byte> bgra(static_cast<std::size_t>(stride) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto offset = static_cast<std::size_t>(y) * stride + x * 4U;
            bgra[offset] = std::byte{static_cast<std::uint8_t>(phase + x)};
            bgra[offset + 1U] =
                std::byte{static_cast<std::uint8_t>(phase + y * 2U)};
            bgra[offset + 2U] =
                std::byte{static_cast<std::uint8_t>(phase + x + y)};
            bgra[offset + 3U] = std::byte{255};
        }
    }
    return capture::DesktopImage{
        std::move(bgra), width, height, stride};
}

synthetic::SyntheticDesktopCaptureScript capture_script() {
    synthetic::SyntheticDesktopCaptureScript script;
    script.output_name = "Synthetic Encoding Pipeline";
    script.initial_width = 64;
    script.initial_height = 64;
    script.steps.emplace_back(patterned_image(17));
    script.steps.emplace_back(patterned_image(83));
    script.steps.emplace_back(patterned_image(149));
    return script;
}

bool contains_annex_b_start_code(const std::vector<std::byte>& bytes) {
    for (std::size_t index = 0; index + 2U < bytes.size(); ++index) {
        if (bytes[index] != std::byte{0} ||
            bytes[index + 1U] != std::byte{0}) {
            continue;
        }
        if (bytes[index + 2U] == std::byte{1}) {
            return true;
        }
        if (index + 3U < bytes.size() &&
            bytes[index + 2U] == std::byte{0} &&
            bytes[index + 3U] == std::byte{1}) {
            return true;
        }
    }
    return false;
}

void synthetic_capture_reaches_the_real_h264_encoder() {
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    domain::CapturedVideoFrameStore frame_store{notifier, 8};
    domain::EncodedVideoAccessUnitQueue access_unit_queue{notifier, 1};

    domain::DefaultVideoEncoderWorker encoder_worker{
        std::make_unique<ffmpeg::FfmpegH264EncoderBackend>(),
        frame_store,
        access_unit_queue,
        notifier};
    domain::DefaultVideoCaptureWorker capture_worker{
        std::make_unique<synthetic::SyntheticDesktopCaptureBackend>(
            capture_script()),
        frame_store,
        notifier};

    domain::VideoEncoderSessionConfig encoder_config;
    encoder_config.encoder.output = {64, 64};
    encoder_config.encoder.frame_rate = {30, 1};
    encoder_config.encoder.target_bit_rate = 300'000;
    encoder_config.encoder.gop_size = 4;

    const auto encoder_started = encoder_worker.start(encoder_config);
    require(encoder_started.has_value(),
            encoder_started ? "encoder must start" :
                              encoder_started.error().message);
    require(encoder_started->encoder.encoder_name == "libx264",
            "the integration pipeline must use the real libx264 encoder");

    domain::VideoCaptureSessionConfig capture_config{
        {},
        domain::SessionTimeline{std::chrono::steady_clock::now()},
        {30, 1},
        5s,
    };
    const auto capture_started = capture_worker.start(capture_config);
    require(capture_started.has_value(),
            capture_started ? "capture must start" :
                              capture_started.error().message);
    require(capture_started->source.output_name ==
                "Synthetic Encoding Pipeline",
            "capture worker must report the scripted source");

    require(wait_until([&] {
                return access_unit_queue.full() &&
                       encoder_worker.stats().access_unit_queue_full_events > 0;
            }),
            "a capacity-one AU queue must apply backpressure before consumption");

    std::mutex collected_mutex;
    std::vector<model::EncodedVideoAccessUnit> collected;
    std::atomic_uint64_t collected_count{0};
    std::jthread collector{[&](const std::stop_token stop_token) {
        while (!stop_token.stop_requested() || !access_unit_queue.empty()) {
            auto access_unit = access_unit_queue.try_pop();
            if (!access_unit) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            {
                std::lock_guard lock{collected_mutex};
                collected.push_back(std::move(*access_unit));
            }
            collected_count.fetch_add(1, std::memory_order_release);
        }
    }};

    require(wait_until([&] {
                return capture_worker.stats().published_frames >= 8 &&
                       encoder_worker.stats().consumed_frames >= 6 &&
                       collected_count.load(std::memory_order_acquire) >= 4;
            }),
            "capture, encoding, and AU consumption must all make progress");

    capture_worker.stop();
    encoder_worker.stop(domain::VideoEncoderStopMode::Drain);
    require(wait_until([&access_unit_queue] {
                return access_unit_queue.empty();
            }),
            "the collector must consume every drained access unit");
    collector.request_stop();
    collector.join();

    const auto capture_stats = capture_worker.stats();
    const auto encoder_stats = encoder_worker.stats();
    require(capture_worker.state() == domain::VideoCaptureWorkerState::Idle &&
                encoder_worker.state() == domain::VideoEncoderWorkerState::Idle,
            "both workers must return to idle after an orderly shutdown");
    require(capture_stats.new_desktop_images == 3 &&
                capture_stats.repeated_frames > 0,
            "the capture worker must encode scripted changes and repeated frames");
    require(capture_stats.fatal_failures == 0 &&
                encoder_stats.fatal_failures == 0,
            "the integration pipeline must complete without a worker failure");
    require(encoder_stats.consumed_frames > 0,
            "drain must consume captured frames before completing");
    require(encoder_stats.access_unit_queue_full_events > 0 &&
                encoder_stats.pending_access_units == 0,
            "the encoder must recover from backpressure without pending output");
    require(encoder_stats.produced_access_units ==
                encoder_stats.submitted_access_units,
            "every produced access unit must reach the queue");

    std::lock_guard lock{collected_mutex};
    require(!collected.empty(), "the pipeline must produce encoded access units");
    require(encoder_stats.submitted_access_units == collected.size(),
            "the collector must receive every submitted access unit");
    require(collected.front().key_frame,
            "the first encoded access unit must be a key frame");

    for (std::size_t index = 0; index < collected.size(); ++index) {
        require(!collected[index].annex_b.empty() &&
                    contains_annex_b_start_code(collected[index].annex_b),
                "every encoded access unit must contain Annex-B H.264 data");
        if (index == 0) {
            continue;
        }
        require(collected[index].presentation_time >
                    collected[index - 1U].presentation_time,
                "encoded presentation times must be strictly increasing");
        require(collected[index].source_sequence >
                    collected[index - 1U].source_sequence,
                "encoded source sequences must be strictly increasing");
    }
}

}  // namespace

int main() {
    try {
        synthetic_capture_reaches_the_real_h264_encoder();
    } catch (const std::exception& error) {
        std::cerr << "publisher video encoding pipeline test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher video encoding pipeline test passed\n";
    return EXIT_SUCCESS;
}
