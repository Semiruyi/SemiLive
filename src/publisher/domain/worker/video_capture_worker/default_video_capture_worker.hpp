#pragma once

#include "publisher/domain/resource/captured_video_frame_store/captured_video_frame_sink.hpp"
#include "publisher/domain/worker/video_capture_worker/video_capture_worker.hpp"
#include "publisher/infrastructure/notifier/notifier.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>

namespace semilive::publisher::domain {

class DefaultVideoCaptureWorker final : public VideoCaptureWorker {
public:
    DefaultVideoCaptureWorker(
        std::unique_ptr<contracts::capture::DesktopCaptureBackend> backend,
        CapturedVideoFrameSink& sink,
        std::shared_ptr<infra::Notifier> notifier);
    ~DefaultVideoCaptureWorker() override;

    DefaultVideoCaptureWorker(const DefaultVideoCaptureWorker&) = delete;
    DefaultVideoCaptureWorker& operator=(const DefaultVideoCaptureWorker&) = delete;
    DefaultVideoCaptureWorker(DefaultVideoCaptureWorker&&) = delete;
    DefaultVideoCaptureWorker& operator=(DefaultVideoCaptureWorker&&) = delete;

    [[nodiscard]] std::expected<VideoCaptureStarted, VideoCaptureWorkerIssue>
    start(VideoCaptureSessionConfig config) override;
    void stop() override;

    [[nodiscard]] VideoCaptureWorkerState state() const noexcept override;
    [[nodiscard]] VideoCaptureWorkerStats stats() const noexcept override;

private:
    using Clock = std::chrono::steady_clock;
    using StartResult = std::expected<VideoCaptureStarted, VideoCaptureWorkerIssue>;
    using ObservationResult =
        std::expected<contracts::capture::DesktopCaptureObservation,
                      VideoCaptureWorkerIssue>;

    enum class ModuleState : std::uint8_t {
        Starting,
        Alive,
        Stopping,
        Stopped,
    };

    enum class FrameContent : std::uint8_t {
        Missing,
        NewImage,
        Repeated,
    };

    struct StartCommand {
        VideoCaptureSessionConfig config;
        std::promise<StartResult> completion;
    };

    struct StopCommand {
        std::promise<void> completion;
    };

    using ControlCommand = std::variant<StartCommand, StopCommand>;

    void worker_main(std::stop_token stop_token) noexcept;
    void worker_loop(std::stop_token stop_token);
    [[nodiscard]] std::optional<ControlCommand>
    wait_for_work(std::stop_token stop_token);
    void process_command(ControlCommand& command) noexcept;
    void process_command(StartCommand& command) noexcept;
    void process_command(StopCommand& command) noexcept;

    [[nodiscard]] bool begin_start() noexcept;
    [[nodiscard]] StartResult start_session(VideoCaptureSessionConfig config) noexcept;
    [[nodiscard]] StartResult start_session_impl(VideoCaptureSessionConfig config);
    [[nodiscard]] std::expected<contracts::capture::DesktopCaptureInfo,
                                VideoCaptureWorkerIssue>
    open_backend(const contracts::capture::DesktopCaptureConfig& config) noexcept;
    void finish_start(bool succeeded) noexcept;
    void cleanup_session() noexcept;

    void process_capture_tick() noexcept;
    [[nodiscard]] std::expected<void, VideoCaptureWorkerIssue>
    process_capture_tick_impl();
    [[nodiscard]] ObservationResult capture_once() noexcept;
    [[nodiscard]] std::expected<FrameContent, VideoCaptureWorkerIssue>
    prepare_frame(contracts::capture::DesktopCaptureObservation observation);
    [[nodiscard]] std::expected<FrameContent, VideoCaptureWorkerIssue>
    adopt_image(contracts::capture::DesktopImage image);
    [[nodiscard]] FrameContent observe_no_change() noexcept;
    [[nodiscard]] std::expected<FrameContent, VideoCaptureWorkerIssue>
    observe_temporary(contracts::capture::DesktopTemporarilyUnavailable unavailable);
    [[nodiscard]] std::expected<void, VideoCaptureWorkerIssue>
    publish_frame(FrameContent content) noexcept;
    [[nodiscard]] std::expected<void, VideoCaptureWorkerIssue>
    advance_schedule() noexcept;

    [[nodiscard]] bool control_command_pending() const noexcept;
    void fail_running(VideoCaptureWorkerIssue issue) noexcept;
    void notify_failure(const VideoCaptureWorkerIssue& issue) noexcept;
    void shutdown_worker() noexcept;
    void cancel_pending_commands() noexcept;
    void mark_module_alive() noexcept;
    void mark_module_stopped(std::string bootstrap_error = {}) noexcept;

    void reset_session_stats(const contracts::capture::DesktopCaptureInfo& source) noexcept;
    void record_processed_tick() noexcept;
    void record_capture_time(Clock::duration elapsed) noexcept;
    void record_image(const model::BgraFrameBuffer& image,
                      bool recovered) noexcept;
    void record_no_change(bool recovered) noexcept;
    void record_temporary(bool new_episode) noexcept;
    void record_published(FrameContent content,
                          CapturedVideoFramePushResult result) noexcept;
    void record_missing_frame() noexcept;
    void record_schedule(std::uint64_t skipped_ticks) noexcept;
    void record_fatal_failure() noexcept;

    std::unique_ptr<contracts::capture::DesktopCaptureBackend> backend_;
    CapturedVideoFrameSink* sink_ = nullptr;
    std::shared_ptr<infra::Notifier> notifier_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ControlCommand> commands_;
    ModuleState module_state_ = ModuleState::Starting;
    VideoCaptureWorkerState session_state_ = VideoCaptureWorkerState::Idle;
    std::string bootstrap_error_;
    VideoCaptureWorkerStats stats_;

    std::optional<FrameScheduler> scheduler_;
    std::optional<FrameTick> next_tick_;
    model::SharedBgraFrameBuffer latest_image_;
    std::optional<Clock::time_point> recovering_since_;
    std::optional<contracts::capture::DesktopCaptureIssue> last_recovery_issue_;
    std::chrono::milliseconds recovery_timeout_{5000};
    std::optional<VideoCaptureWorkerIssue> last_issue_;
    std::uint64_t next_sequence_ = 0;
    bool backend_open_ = false;

    std::jthread worker_;
};

}  // namespace semilive::publisher::domain
