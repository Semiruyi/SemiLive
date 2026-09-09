#pragma once

#include <semilive/publisher/domain/worker/video_encoder_worker/video_encoder_worker.hpp>
#include <semilive/publisher/contracts/notifier/notifier.hpp>

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

class CapturedVideoFrameSource;
class EncodedVideoAccessUnitSink;

class DefaultVideoEncoderWorker final : public VideoEncoderWorker {
public:
    DefaultVideoEncoderWorker(
        std::unique_ptr<contracts::encoder::VideoEncoderBackend> backend,
        CapturedVideoFrameSource& source,
        EncodedVideoAccessUnitSink& sink,
        std::shared_ptr<contracts::Notifier> notifier);
    ~DefaultVideoEncoderWorker() override;

    DefaultVideoEncoderWorker(const DefaultVideoEncoderWorker&) = delete;
    DefaultVideoEncoderWorker& operator=(const DefaultVideoEncoderWorker&) = delete;
    DefaultVideoEncoderWorker(DefaultVideoEncoderWorker&&) = delete;
    DefaultVideoEncoderWorker& operator=(DefaultVideoEncoderWorker&&) = delete;

    [[nodiscard]] std::expected<VideoEncoderStarted, VideoEncoderWorkerIssue>
    start(VideoEncoderSessionConfig config) override;
    void stop(VideoEncoderStopMode mode) override;

    [[nodiscard]] VideoEncoderWorkerState state() const noexcept override;
    [[nodiscard]] VideoEncoderWorkerStats stats() const noexcept override;

private:
    using Clock = std::chrono::steady_clock;
    using StartResult =
        std::expected<VideoEncoderStarted, VideoEncoderWorkerIssue>;

    enum class ModuleState : std::uint8_t {
        Starting,
        Alive,
        Stopping,
        Stopped,
    };

    struct StartCommand {
        VideoEncoderSessionConfig config;
        std::promise<StartResult> completion;
    };

    struct StopCommand {
        VideoEncoderStopMode mode = VideoEncoderStopMode::Abort;
        std::promise<void> completion;
    };

    using ControlCommand = std::variant<StartCommand, StopCommand>;

    void worker_main(std::stop_token stop_token) noexcept;
    void worker_loop(std::stop_token stop_token);
    [[nodiscard]] std::optional<ControlCommand> try_take_command();
    void wait_for_signal(std::stop_token stop_token);
    void process_command(ControlCommand& command) noexcept;
    void process_command(StartCommand& command) noexcept;
    void process_command(StopCommand& command) noexcept;

    [[nodiscard]] bool begin_start() noexcept;
    [[nodiscard]] StartResult start_session(
        VideoEncoderSessionConfig config) noexcept;
    void finish_start(bool succeeded) noexcept;
    [[nodiscard]] bool process_session_step() noexcept;
    [[nodiscard]] bool process_session_step_impl();
    [[nodiscard]] bool submit_pending_access_unit();
    [[nodiscard]] bool consume_and_encode_frame(
        model::CapturedVideoFrame frame,
        bool draining);
    [[nodiscard]] bool flush_backend();
    void append_batch(contracts::encoder::VideoEncodeBatch batch,
                      Clock::duration backend_time,
                      bool encode_call,
                      bool flush_batch);
    void finish_drain() noexcept;
    void abort_session() noexcept;
    void cleanup_session(bool count_abandoned_pending) noexcept;
    void fail_session(VideoEncoderWorkerIssue issue) noexcept;
    void notify_failure(const VideoEncoderWorkerIssue& issue) noexcept;

    void subscribe_to_resources();
    void signal_frame_available() noexcept;
    void signal_queue_available() noexcept;
    void disable_notifications() noexcept;
    void begin_backpressure() noexcept;
    void finish_backpressure() noexcept;
    void complete_drain_waiters() noexcept;
    void shutdown_worker() noexcept;
    void cancel_pending_commands() noexcept;
    void mark_module_alive() noexcept;
    void mark_module_stopped(std::string bootstrap_error = {}) noexcept;

    void record_started(const contracts::encoder::VideoEncoderInfo& info);
    void record_consumed_frame(const model::CapturedVideoFrame& frame,
                               bool draining) noexcept;
    void record_backend_batch(const contracts::encoder::VideoEncodeBatch& batch,
                              Clock::duration backend_time,
                              bool encode_call,
                              bool flush_batch) noexcept;
    void record_capture_to_encode(Clock::time_point captured_at) noexcept;
    void record_submitted_access_unit() noexcept;
    void update_pending_count() noexcept;

    std::unique_ptr<contracts::encoder::VideoEncoderBackend> backend_;
    CapturedVideoFrameSource* source_ = nullptr;
    EncodedVideoAccessUnitSink* sink_ = nullptr;
    std::shared_ptr<contracts::Notifier> notifier_;

    mutable std::mutex callback_mutex_;
    bool callbacks_enabled_ = true;
    std::shared_ptr<contracts::Notifier::Subscription> frame_subscription_;
    std::shared_ptr<contracts::Notifier::Subscription> queue_subscription_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ControlCommand> commands_;
    ModuleState module_state_ = ModuleState::Starting;
    VideoEncoderWorkerState session_state_ = VideoEncoderWorkerState::Idle;
    std::string bootstrap_error_;
    VideoEncoderWorkerStats stats_;
    bool frame_available_hint_ = false;
    bool queue_available_hint_ = false;

    std::deque<model::EncodedVideoAccessUnit> pending_access_units_;
    std::deque<std::promise<void>> drain_waiters_;
    std::optional<std::uint64_t> previous_input_sequence_;
    std::optional<Clock::time_point> backpressure_started_at_;
    bool backend_open_ = false;
    bool backend_flushed_ = false;

    std::jthread worker_;
};

}  // namespace semilive::publisher::domain
