#pragma once

#include <semilive/publisher/contracts/notifier/notifier.hpp>
#include <semilive/publisher/domain/worker/video_output_worker/video_output_worker.hpp>

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

class EncodedVideoAccessUnitSource;

class DefaultVideoOutputWorker final : public VideoOutputWorker {
public:
    DefaultVideoOutputWorker(
        std::unique_ptr<contracts::output::VideoAccessUnitOutputBackend> backend,
        EncodedVideoAccessUnitSource& source,
        std::shared_ptr<contracts::Notifier> notifier);
    ~DefaultVideoOutputWorker() override;

    DefaultVideoOutputWorker(const DefaultVideoOutputWorker&) = delete;
    DefaultVideoOutputWorker& operator=(const DefaultVideoOutputWorker&) = delete;
    DefaultVideoOutputWorker(DefaultVideoOutputWorker&&) = delete;
    DefaultVideoOutputWorker& operator=(DefaultVideoOutputWorker&&) = delete;

    [[nodiscard]] std::expected<VideoOutputStarted, VideoOutputWorkerIssue>
    start() override;
    void stop(VideoOutputStopMode mode) override;

    [[nodiscard]] VideoOutputWorkerState state() const noexcept override;
    [[nodiscard]] VideoOutputWorkerStats stats() const noexcept override;

private:
    using Clock = std::chrono::steady_clock;
    using StartResult =
        std::expected<VideoOutputStarted, VideoOutputWorkerIssue>;

    enum class ModuleState : std::uint8_t {
        Starting,
        Alive,
        Stopping,
        Stopped,
    };

    struct StartCommand {
        std::promise<StartResult> completion;
    };

    struct StopCommand {
        VideoOutputStopMode mode = VideoOutputStopMode::Abort;
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
    [[nodiscard]] StartResult start_session() noexcept;
    void finish_start(bool succeeded) noexcept;
    [[nodiscard]] bool process_session_step() noexcept;
    [[nodiscard]] bool process_session_step_impl();
    [[nodiscard]] bool consume_access_unit(
        model::EncodedVideoAccessUnit access_unit,
        bool draining);
    [[nodiscard]] std::optional<VideoOutputWorkerIssue>
    validate_access_unit(const model::EncodedVideoAccessUnit& access_unit) const;
    [[nodiscard]] bool flush_backend();
    void finish_drain() noexcept;
    void abort_session() noexcept;
    void cleanup_session() noexcept;
    void fail_session(VideoOutputWorkerIssue issue) noexcept;
    void notify_failure(const VideoOutputWorkerIssue& issue) noexcept;

    void subscribe_to_resource();
    void signal_access_unit_available() noexcept;
    void disable_notifications() noexcept;
    void complete_drain_waiters() noexcept;
    void shutdown_worker() noexcept;
    void cancel_pending_commands() noexcept;
    void mark_module_alive() noexcept;
    void mark_module_stopped(std::string bootstrap_error = {}) noexcept;

    void record_started(const contracts::output::VideoOutputInfo& info);
    void record_consumed_access_unit(
        const model::EncodedVideoAccessUnit& access_unit,
        bool draining) noexcept;
    void record_backend_result(
        const contracts::output::VideoOutputReceipt& receipt,
        Clock::duration backend_time,
        bool flush_call) noexcept;
    void record_capture_to_output(Clock::time_point captured_at) noexcept;

    std::unique_ptr<contracts::output::VideoAccessUnitOutputBackend> backend_;
    EncodedVideoAccessUnitSource* source_ = nullptr;
    std::shared_ptr<contracts::Notifier> notifier_;

    mutable std::mutex callback_mutex_;
    bool callbacks_enabled_ = true;
    std::shared_ptr<contracts::Notifier::Subscription> source_subscription_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ControlCommand> commands_;
    ModuleState module_state_ = ModuleState::Starting;
    VideoOutputWorkerState session_state_ = VideoOutputWorkerState::Idle;
    std::string bootstrap_error_;
    VideoOutputWorkerStats stats_;
    bool access_unit_available_hint_ = false;

    std::deque<std::promise<void>> drain_waiters_;
    std::optional<model::MediaTime> previous_presentation_time_;
    std::optional<std::uint64_t> previous_source_sequence_;
    bool backend_open_ = false;
    bool backend_flushed_ = false;

    std::jthread worker_;
};

}  // namespace semilive::publisher::domain
