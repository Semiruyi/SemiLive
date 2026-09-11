#include <semilive/publisher/composition/publisher_composition.hpp>

#include <semilive/publisher/application/publisher_controller/default_publisher_controller.hpp>
#include <semilive/publisher/contracts/notifier/notifier.hpp>
#include <semilive/publisher/domain/resource/captured_video_frame_store/captured_video_frame_store.hpp>
#include <semilive/publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue.hpp>
#include <semilive/publisher/domain/worker/video_capture_worker/default_video_capture_worker.hpp>
#include <semilive/publisher/domain/worker/video_encoder_worker/default_video_encoder_worker.hpp>
#include <semilive/publisher/domain/worker/video_output_worker/default_video_output_worker.hpp>
#include <semilive/publisher/infrastructure/ffmpeg/video_encoder/ffmpeg_h264_encoder_backend.hpp>
#include <semilive/publisher/infrastructure/notifier/default_notifier.hpp>
#include <semilive/publisher/infrastructure/output/h264_file_output_backend.hpp>

#if defined(_WIN32)
#include <semilive/publisher/infrastructure/capture/dxgi_desktop_capture_backend.hpp>
#endif

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace semilive::publisher::composition {
namespace {

PublisherCompositionIssue make_issue(
    const PublisherCompositionOperation operation,
    std::string message) {
    return {operation, std::nullopt, std::move(message)};
}

PublisherCompositionIssue make_controller_issue(
    application::PublisherControllerIssue issue) {
    auto message = issue.message;
    return {PublisherCompositionOperation::StopSession,
            std::move(issue),
            std::move(message)};
}

}  // namespace

struct PublisherComposition::Impl {
    enum class State {
        Unassembled,
        Assembled,
        Disposed,
    };

    explicit Impl(PublisherConfig config) : config_{std::move(config)} {}

    ~Impl() {
        dispose_noexcept();
    }

    [[nodiscard]] PublisherCompositionResult assemble();
    [[nodiscard]] application::PublisherController* controller() noexcept;
    [[nodiscard]] const application::PublisherController*
    controller() const noexcept;
    [[nodiscard]] PublisherCompositionResult dispose();

    void reset_graph() noexcept;
    void dispose_noexcept() noexcept;

    PublisherConfig config_;
    State state_ = State::Unassembled;

    std::shared_ptr<contracts::Notifier> notifier_;
    std::unique_ptr<domain::CapturedVideoFrameStore> frame_store_;
    std::unique_ptr<domain::EncodedVideoAccessUnitQueue> access_unit_queue_;
    std::unique_ptr<domain::DefaultVideoOutputWorker> output_worker_;
    std::unique_ptr<domain::DefaultVideoEncoderWorker> encoder_worker_;
    std::unique_ptr<domain::DefaultVideoCaptureWorker> capture_worker_;
    std::unique_ptr<application::DefaultPublisherController> controller_;
};

PublisherCompositionResult PublisherComposition::Impl::assemble() {
    if (state_ == State::Assembled) {
        return std::unexpected{make_issue(
            PublisherCompositionOperation::Control,
            "publisher composition is already assembled")};
    }
    if (state_ == State::Disposed) {
        return std::unexpected{make_issue(
            PublisherCompositionOperation::Control,
            "disposed publisher composition cannot be assembled")};
    }
    if (config_.output.path.empty()) {
        return std::unexpected{make_issue(
            PublisherCompositionOperation::ValidateConfig,
            "publisher H.264 output path must not be empty")};
    }
    if (config_.video.recovery_timeout <=
        std::chrono::milliseconds::zero()) {
        return std::unexpected{make_issue(
            PublisherCompositionOperation::ValidateConfig,
            "publisher video recovery timeout must be positive")};
    }

#if !defined(_WIN32)
    return std::unexpected{make_issue(
        PublisherCompositionOperation::CheckPlatform,
        "DXGI desktop publishing is only supported on Windows")};
#else
    auto operation = PublisherCompositionOperation::CreateNotifier;
    try {
        notifier_ = std::make_shared<infra::DefaultNotifier>();

        operation = PublisherCompositionOperation::CreateResources;
        frame_store_ =
            std::make_unique<domain::CapturedVideoFrameStore>(notifier_);
        access_unit_queue_ =
            std::make_unique<domain::EncodedVideoAccessUnitQueue>(notifier_);

        operation = PublisherCompositionOperation::CreateOutputWorker;
        output_worker_ = std::make_unique<domain::DefaultVideoOutputWorker>(
            std::make_unique<infra::output::H264FileOutputBackend>(
                config_.output.path),
            *access_unit_queue_, notifier_);

        operation = PublisherCompositionOperation::CreateEncoderWorker;
        encoder_worker_ = std::make_unique<domain::DefaultVideoEncoderWorker>(
            std::make_unique<infra::ffmpeg::FfmpegH264EncoderBackend>(),
            *frame_store_, *access_unit_queue_, notifier_);

        operation = PublisherCompositionOperation::CreateCaptureWorker;
        capture_worker_ = std::make_unique<domain::DefaultVideoCaptureWorker>(
            std::make_unique<infra::capture::DxgiDesktopCaptureBackend>(),
            *frame_store_, notifier_);

        operation = PublisherCompositionOperation::CreateController;
        application::PublisherVideoSessionPlan plan{
            config_.video.capture,
            config_.video.recovery_timeout,
            config_.video.encoder,
        };
        application::PublisherVideoPipeline pipeline{
            *capture_worker_,
            *encoder_worker_,
            *output_worker_,
            *frame_store_,
            *access_unit_queue_,
        };
        controller_ =
            std::make_unique<application::DefaultPublisherController>(
                std::move(plan), pipeline, notifier_);
        state_ = State::Assembled;
        return {};
    } catch (const std::exception& error) {
        reset_graph();
        return std::unexpected{make_issue(operation, error.what())};
    } catch (...) {
        reset_graph();
        return std::unexpected{
            make_issue(operation,
                       "unknown exception while assembling publisher")};
    }
#endif
}

application::PublisherController*
PublisherComposition::Impl::controller() noexcept {
    if (state_ != State::Assembled) {
        return nullptr;
    }
    return controller_.get();
}

const application::PublisherController*
PublisherComposition::Impl::controller() const noexcept {
    if (state_ != State::Assembled) {
        return nullptr;
    }
    return controller_.get();
}

PublisherCompositionResult PublisherComposition::Impl::dispose() {
    if (state_ == State::Disposed) {
        return {};
    }

    state_ = State::Disposed;
    std::optional<PublisherCompositionIssue> issue;
    if (controller_ &&
        controller_->state() != application::PublisherControllerState::Idle) {
        try {
            auto stopped = controller_->stop_publishing();
            if (!stopped) {
                issue = make_controller_issue(std::move(stopped.error()));
            }
        } catch (const std::exception& error) {
            issue = make_issue(PublisherCompositionOperation::StopSession,
                               error.what());
        } catch (...) {
            issue = make_issue(
                PublisherCompositionOperation::StopSession,
                "unknown exception while stopping publisher during disposal");
        }
    }

    reset_graph();
    if (issue) {
        return std::unexpected{std::move(*issue)};
    }
    return {};
}

void PublisherComposition::Impl::reset_graph() noexcept {
    controller_.reset();
    capture_worker_.reset();
    encoder_worker_.reset();
    output_worker_.reset();
    access_unit_queue_.reset();
    frame_store_.reset();
    notifier_.reset();
}

void PublisherComposition::Impl::dispose_noexcept() noexcept {
    try {
        (void)dispose();
    } catch (...) {
        state_ = State::Disposed;
        reset_graph();
    }
}

PublisherComposition::PublisherComposition(PublisherConfig config)
    : impl_{std::make_unique<Impl>(std::move(config))} {}

PublisherComposition::~PublisherComposition() = default;

PublisherCompositionResult PublisherComposition::assemble() {
    return impl_->assemble();
}

application::PublisherController*
PublisherComposition::controller() noexcept {
    return impl_->controller();
}

const application::PublisherController*
PublisherComposition::controller() const noexcept {
    return impl_->controller();
}

PublisherCompositionResult PublisherComposition::dispose() {
    return impl_->dispose();
}

}  // namespace semilive::publisher::composition
