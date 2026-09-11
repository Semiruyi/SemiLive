#include <semilive/publisher/application/publisher_controller/default_publisher_controller.hpp>
#include <semilive/publisher/domain/resource/captured_video_frame_store/captured_video_frame_store.hpp>
#include <semilive/publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue.hpp>
#include <semilive/publisher/domain/worker/video_capture_worker/default_video_capture_worker.hpp>
#include <semilive/publisher/domain/worker/video_encoder_worker/default_video_encoder_worker.hpp>
#include <semilive/publisher/domain/worker/video_output_worker/default_video_output_worker.hpp>
#include <semilive/publisher/infrastructure/capture/synthetic_desktop_capture_backend.hpp>
#include <semilive/publisher/infrastructure/ffmpeg/video_encoder/ffmpeg_h264_encoder_backend.hpp>
#include <semilive/publisher/infrastructure/notifier/default_notifier.hpp>
#include <semilive/publisher/infrastructure/output/h264_file_output_backend.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace capture = semilive::publisher::contracts::capture;
namespace app = semilive::publisher::application;
namespace domain = semilive::publisher::domain;
namespace ffmpeg = semilive::publisher::infra::ffmpeg;
namespace infra = semilive::publisher::infra;
namespace output = semilive::publisher::infra::output;
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

class TemporaryOutputFile {
public:
    TemporaryOutputFile() {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::current_path() /
                ("semilive_video_file_pipeline_" + std::to_string(nonce) +
                 ".h264");
    }

    ~TemporaryOutputFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryOutputFile(const TemporaryOutputFile&) = delete;
    TemporaryOutputFile& operator=(const TemporaryOutputFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

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
    script.output_name = "Synthetic File Output Pipeline";
    script.initial_width = 64;
    script.initial_height = 64;
    script.steps.emplace_back(patterned_image(23));
    script.steps.emplace_back(patterned_image(101));
    script.steps.emplace_back(patterned_image(179));
    return script;
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    require(input.is_open(), "generated H.264 file must be readable");
    const std::vector<char> chars{std::istreambuf_iterator<char>{input},
                                  std::istreambuf_iterator<char>{}};

    std::vector<std::byte> bytes;
    bytes.reserve(chars.size());
    for (const char value : chars) {
        bytes.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(value)));
    }
    return bytes;
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

void synthetic_capture_reaches_a_real_h264_file() {
    TemporaryOutputFile output_file;
    auto notifier = std::make_shared<infra::DefaultNotifier>();
    domain::CapturedVideoFrameStore frame_store{notifier, 8};
    domain::EncodedVideoAccessUnitQueue access_unit_queue{notifier, 2};

    domain::DefaultVideoOutputWorker output_worker{
        std::make_unique<output::H264FileOutputBackend>(output_file.path()),
        access_unit_queue,
        notifier};
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

    app::PublisherVideoSessionPlan plan;
    plan.capture = {};
    plan.recovery_timeout = 5s;
    plan.encoder.output = {64, 64};
    plan.encoder.frame_rate = {30, 1};
    plan.encoder.target_bit_rate = 300'000;
    plan.encoder.gop_size = 4;

    app::DefaultPublisherController controller{
        plan,
        {capture_worker,
         encoder_worker,
         output_worker,
         frame_store,
         access_unit_queue},
        notifier};

    const auto started = controller.start_publishing();
    require(started.has_value(),
            started ? "publisher must start" : started.error().message);
    require(started->output.output.output_name == output_file.path().string(),
            "controller must report the configured H.264 file");
    require(started->encoder.encoder.encoder_name == "libx264",
            "the file pipeline must use the real libx264 encoder");
    require(started->capture.source.output_name ==
                "Synthetic File Output Pipeline",
            "controller must preserve capture startup information");

    require(wait_until([&] {
                return capture_worker.stats().published_frames >= 8 &&
                       output_worker.stats().consumed_access_units >= 4;
            }),
            "capture, encoding, and file output must all make progress");

    const auto stopped = controller.stop_publishing();
    require(stopped.has_value(),
            stopped ? "publisher must stop" : stopped.error().message);

    const auto capture_stats = capture_worker.stats();
    const auto encoder_stats = encoder_worker.stats();
    const auto output_stats = output_worker.stats();

    require(controller.state() == app::PublisherControllerState::Idle &&
                capture_worker.state() == domain::VideoCaptureWorkerState::Idle &&
                encoder_worker.state() ==
                    domain::VideoEncoderWorkerState::Idle &&
                output_worker.state() == domain::VideoOutputWorkerState::Idle,
            "all pipeline workers must return to idle after ordered drain");
    require(frame_store.empty() && access_unit_queue.empty(),
            "ordered drain must leave both inter-stage resources empty");
    require(capture_stats.fatal_failures == 0 &&
                encoder_stats.fatal_failures == 0 &&
                output_stats.fatal_failures == 0,
            "the file pipeline must complete without worker failures");
    require(capture_stats.new_desktop_images == 3 &&
                capture_stats.repeated_frames > 0,
            "the file pipeline must include changed and repeated frames");
    require(encoder_stats.produced_access_units ==
                encoder_stats.submitted_access_units &&
                encoder_stats.submitted_access_units ==
                    output_stats.consumed_access_units,
            "every encoded access unit must reach the file output worker");
    require(output_stats.consumed_access_units > 0 &&
                output_stats.key_frames > 0,
            "file output must consume encoded data including a key frame");
    require(output_stats.emitted_units ==
                output_stats.consumed_access_units &&
                output_stats.emitted_bytes == output_stats.input_bytes,
            "file backend receipts must account for every AU and input byte");
    require(output_stats.flush_calls == 1,
            "normal output drain must flush the file exactly once");
    require(controller.stats().completed_sessions == 1 &&
                controller.stats().failed_sessions == 0,
            "controller must report one completed file-output session");

    std::error_code file_size_error;
    const auto file_size = std::filesystem::file_size(
        output_file.path(), file_size_error);
    require(!file_size_error && file_size == output_stats.emitted_bytes,
            "generated file size must equal the output byte statistics");

    const auto bytes = read_bytes(output_file.path());
    require(!bytes.empty(), "the complete pipeline must generate H.264 bytes");
    require(contains_annex_b_start_code(bytes),
            "generated file must contain Annex-B H.264 start codes");
}

}  // namespace

int main() {
    try {
        synthetic_capture_reaches_a_real_h264_file();
    } catch (const std::exception& error) {
        std::cerr << "publisher video file output pipeline test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher video file output pipeline test passed\n";
    return EXIT_SUCCESS;
}
