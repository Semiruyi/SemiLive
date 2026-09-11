#include <semilive/publisher/application/publisher_controller/default_publisher_controller.hpp>

#include <semilive/publisher/domain/worker/video_capture_worker/video_capture_worker_events.hpp>
#include <semilive/publisher/domain/worker/video_encoder_worker/video_encoder_worker_events.hpp>
#include <semilive/publisher/domain/worker/video_output_worker/video_output_worker_events.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace semilive::publisher::application {
namespace {

using namespace std::chrono_literals;

PublisherControllerIssue control_issue(std::string message) {
    return {
        PublisherControllerOperation::Control,
        std::monostate{},
        std::move(message),
    };
}

PublisherControllerIssue internal_issue(
    const PublisherControllerOperation operation,
    std::string message) {
    return {operation, std::monostate{}, std::move(message)};
}

PublisherControllerIssue capture_issue(
    const PublisherControllerOperation operation,
    domain::VideoCaptureWorkerIssue issue) {
    auto message = issue.message;
    return {operation, std::move(issue), std::move(message)};
}

PublisherControllerIssue encoder_issue(
    const PublisherControllerOperation operation,
    domain::VideoEncoderWorkerIssue issue) {
    auto message = issue.message;
    return {operation, std::move(issue), std::move(message)};
}

PublisherControllerIssue output_issue(
    const PublisherControllerOperation operation,
    domain::VideoOutputWorkerIssue issue) {
    auto message = issue.message;
    return {operation, std::move(issue), std::move(message)};
}

}  // namespace

struct DefaultPublisherController::Impl {
    enum class DrainStage : std::uint8_t {
        Encoder,
        Output,
    };

    struct StartCommand {
        std::promise<PublisherStartResult> completion;
    };

    struct StopCommand {
        std::promise<PublisherStopResult> completion;
    };

    struct FailureCommand {
        std::uint64_t session_id = 0;
        PublisherControllerIssue issue;
    };

    struct DrainCompletedCommand {
        std::uint64_t session_id = 0;
        DrainStage stage = DrainStage::Encoder;
        std::optional<PublisherControllerIssue> issue;
    };

    struct ShutdownCommand {
        std::promise<void> completion;
    };

    using ControlCommand =
        std::variant<StartCommand,
                     StopCommand,
                     FailureCommand,
                     DrainCompletedCommand,
                     ShutdownCommand>;

    struct ClearedResources {
        std::size_t frames = 0;
        std::size_t access_units = 0;
    };

    Impl(PublisherVideoSessionPlan plan,
         const PublisherVideoPipeline pipeline,
         std::shared_ptr<contracts::Notifier> notifier)
        : plan_{std::move(plan)},
          capture_worker_{&pipeline.capture_worker},
          encoder_worker_{&pipeline.encoder_worker},
          output_worker_{&pipeline.output_worker},
          frame_store_{&pipeline.frame_store},
          access_unit_queue_{&pipeline.access_unit_queue},
          notifier_{std::move(notifier)} {
        if (!notifier_) {
            throw std::invalid_argument{
                "publisher controller notifier must not be null"};
        }
        subscribe_to_failures();
        controller_thread_ = std::jthread{
            [this](const std::stop_token stop_token) {
                controller_main(stop_token);
            }};
    }

    ~Impl() {
        disable_failure_callbacks();
        shutdown();
    }

    [[nodiscard]] PublisherStartResult start_publishing();
    [[nodiscard]] PublisherStopResult stop_publishing();
    [[nodiscard]] PublisherControllerState state() const noexcept;
    [[nodiscard]] PublisherControllerStats stats() const noexcept;
    [[nodiscard]] PublisherWaitResult wait_for_terminal_for(
        std::chrono::milliseconds timeout);

    void controller_main(std::stop_token stop_token) noexcept;
    [[nodiscard]] std::optional<ControlCommand> wait_for_command(
        std::stop_token stop_token);
    void process_command(ControlCommand& command) noexcept;
    void process_command(StartCommand& command) noexcept;
    void process_command(StopCommand& command) noexcept;
    void process_command(FailureCommand& command) noexcept;
    void process_command(DrainCompletedCommand& command) noexcept;
    void process_command(ShutdownCommand& command) noexcept;

    void start_session(StartCommand& command);
    void fail_start(StartCommand& command, PublisherControllerIssue issue);
    void begin_normal_stop(StopCommand command);
    void launch_drain(DrainStage stage);
    void finish_normal_stop();
    void fail_running_session(PublisherControllerIssue issue) noexcept;
    void abort_started_pipeline() noexcept;
    [[nodiscard]] ClearedResources clear_resources() noexcept;
    void complete_stop_waiters(PublisherStopResult result) noexcept;
    void join_drain_thread() noexcept;

    void subscribe_to_failures();
    void enqueue_failure(PublisherControllerIssue issue) noexcept;
    void enqueue_drain_completed(DrainCompletedCommand command) noexcept;
    void disable_failure_callbacks() noexcept;
    void shutdown() noexcept;
    void cancel_pending_commands() noexcept;

    void set_state(PublisherControllerState state) noexcept;
    [[nodiscard]] PublisherControllerIssue current_worker_failure(
        DrainStage stage) const;

    PublisherVideoSessionPlan plan_;
    domain::VideoCaptureWorker* capture_worker_ = nullptr;
    domain::VideoEncoderWorker* encoder_worker_ = nullptr;
    domain::VideoOutputWorker* output_worker_ = nullptr;
    domain::CapturedVideoFrameStoreControl* frame_store_ = nullptr;
    domain::EncodedVideoAccessUnitQueueControl* access_unit_queue_ = nullptr;
    std::shared_ptr<contracts::Notifier> notifier_;

    mutable std::mutex callback_mutex_;
    bool failure_callbacks_enabled_ = true;
    std::shared_ptr<contracts::Notifier::Subscription> capture_subscription_;
    std::shared_ptr<contracts::Notifier::Subscription> encoder_subscription_;
    std::shared_ptr<contracts::Notifier::Subscription> output_subscription_;

    mutable std::mutex mutex_;
    std::condition_variable_any command_cv_;
    std::condition_variable state_cv_;
    std::deque<ControlCommand> commands_;
    bool accepting_commands_ = true;
    bool exit_requested_ = false;
    PublisherControllerState state_ = PublisherControllerState::Idle;
    std::uint64_t session_id_ = 0;
    std::uint64_t started_sessions_ = 0;
    std::uint64_t completed_sessions_ = 0;
    std::uint64_t failed_sessions_ = 0;
    std::uint64_t start_failures_ = 0;
    std::uint64_t cleared_frames_ = 0;
    std::uint64_t cleared_access_units_ = 0;
    std::optional<PublisherControllerIssue> last_issue_;
    std::optional<std::uint64_t> queued_failure_session_;
    ClearedResources last_cleanup_;

    bool capture_started_ = false;
    bool encoder_started_ = false;
    bool output_started_ = false;
    bool failure_recorded_for_session_ = false;
    std::vector<std::promise<PublisherStopResult>> stop_waiters_;
    std::jthread drain_thread_;
    std::jthread controller_thread_;
};

PublisherStartResult DefaultPublisherController::Impl::start_publishing() {
    if (std::this_thread::get_id() == controller_thread_.get_id()) {
        return std::unexpected{control_issue(
            "publisher controller cannot start itself from its control thread")};
    }

    StartCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock{mutex_};
        if (!accepting_commands_) {
            return std::unexpected{control_issue(
                "publisher controller is shutting down")};
        }
        commands_.emplace_back(std::move(command));
    }
    command_cv_.notify_one();
    return completion.get();
}

PublisherStopResult DefaultPublisherController::Impl::stop_publishing() {
    if (std::this_thread::get_id() == controller_thread_.get_id()) {
        return std::unexpected{control_issue(
            "publisher controller cannot stop itself from its control thread")};
    }

    StopCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock{mutex_};
        if (!accepting_commands_) {
            return std::unexpected{control_issue(
                "publisher controller is shutting down")};
        }
        commands_.emplace_back(std::move(command));
    }
    command_cv_.notify_one();
    return completion.get();
}

PublisherControllerState DefaultPublisherController::Impl::state() const noexcept {
    std::lock_guard lock{mutex_};
    return state_;
}

PublisherControllerStats DefaultPublisherController::Impl::stats() const noexcept {
    PublisherControllerStats result;
    {
        std::lock_guard lock{mutex_};
        result.state = state_;
        result.session_id = session_id_;
        result.started_sessions = started_sessions_;
        result.completed_sessions = completed_sessions_;
        result.failed_sessions = failed_sessions_;
        result.start_failures = start_failures_;
        result.cleared_frames = cleared_frames_;
        result.cleared_access_units = cleared_access_units_;
        result.last_issue = last_issue_;
    }

    result.frame_store_size = frame_store_->size();
    result.frame_store_peak_size = frame_store_->peak_size();
    result.access_unit_queue_size = access_unit_queue_->size();
    result.access_unit_queue_peak_size = access_unit_queue_->peak_size();
    result.capture = capture_worker_->stats();
    result.encoder = encoder_worker_->stats();
    result.output = output_worker_->stats();
    return result;
}

PublisherWaitResult DefaultPublisherController::Impl::wait_for_terminal_for(
    const std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    const auto terminal = [this] {
        return state_ == PublisherControllerState::Idle ||
               state_ == PublisherControllerState::Failed;
    };
    if (!state_cv_.wait_for(lock, std::max(timeout, 0ms), terminal)) {
        return {PublisherWaitStatus::Timeout, std::nullopt};
    }
    if (state_ == PublisherControllerState::Failed) {
        return {PublisherWaitStatus::Failed, last_issue_};
    }
    return {PublisherWaitStatus::Idle, std::nullopt};
}

void DefaultPublisherController::Impl::controller_main(
    const std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested() && !exit_requested_) {
        auto command = wait_for_command(stop_token);
        if (!command) {
            continue;
        }
        process_command(*command);
    }

    abort_started_pipeline();
    join_drain_thread();
    (void)clear_resources();
    cancel_pending_commands();
}

std::optional<DefaultPublisherController::Impl::ControlCommand>
DefaultPublisherController::Impl::wait_for_command(
    const std::stop_token stop_token) {
    std::unique_lock lock{mutex_};
    command_cv_.wait(lock, stop_token, [this] {
        return !commands_.empty() || exit_requested_;
    });
    if (commands_.empty()) {
        return std::nullopt;
    }

    auto command = std::move(commands_.front());
    commands_.pop_front();
    return command;
}

void DefaultPublisherController::Impl::process_command(
    ControlCommand& command) noexcept {
    std::visit([this](auto& value) { process_command(value); }, command);
}

void DefaultPublisherController::Impl::process_command(
    StartCommand& command) noexcept {
    try {
        start_session(command);
    } catch (const std::exception& error) {
        fail_start(command, internal_issue(PublisherControllerOperation::Internal,
                                           error.what()));
    } catch (...) {
        fail_start(command, internal_issue(
                                PublisherControllerOperation::Internal,
                                "unknown exception while starting publisher"));
    }
}

void DefaultPublisherController::Impl::process_command(
    StopCommand& command) noexcept {
    const auto current_state = state();
    if (current_state == PublisherControllerState::Idle) {
        command.completion.set_value(PublisherStopped{session_id_, 0, 0});
        return;
    }
    if (current_state == PublisherControllerState::Failed) {
        const auto cleanup = last_cleanup_;
        set_state(PublisherControllerState::Idle);
        command.completion.set_value(
            PublisherStopped{session_id_, cleanup.frames,
                             cleanup.access_units});
        return;
    }
    if (current_state == PublisherControllerState::Stopping) {
        stop_waiters_.push_back(std::move(command.completion));
        return;
    }
    if (current_state != PublisherControllerState::Running) {
        command.completion.set_value(std::unexpected{control_issue(
            "publisher can only stop from running, failed, or idle state")});
        return;
    }

    begin_normal_stop(std::move(command));
}

void DefaultPublisherController::Impl::process_command(
    FailureCommand& command) noexcept {
    if (command.session_id != session_id_) {
        return;
    }
    const auto current_state = state();
    if (current_state != PublisherControllerState::Running &&
        current_state != PublisherControllerState::Stopping &&
        current_state != PublisherControllerState::Starting) {
        return;
    }
    fail_running_session(std::move(command.issue));
}

void DefaultPublisherController::Impl::process_command(
    DrainCompletedCommand& command) noexcept {
    if (command.session_id != session_id_ ||
        state() != PublisherControllerState::Stopping) {
        join_drain_thread();
        return;
    }

    join_drain_thread();
    if (command.issue) {
        fail_running_session(std::move(*command.issue));
        return;
    }

    if (command.stage == DrainStage::Encoder) {
        if (encoder_worker_->state() != domain::VideoEncoderWorkerState::Idle) {
            fail_running_session(current_worker_failure(DrainStage::Encoder));
            return;
        }
        encoder_started_ = false;
        launch_drain(DrainStage::Output);
        return;
    }

    if (output_worker_->state() != domain::VideoOutputWorkerState::Idle) {
        fail_running_session(current_worker_failure(DrainStage::Output));
        return;
    }
    output_started_ = false;
    finish_normal_stop();
}

void DefaultPublisherController::Impl::process_command(
    ShutdownCommand& command) noexcept {
    abort_started_pipeline();
    join_drain_thread();
    last_cleanup_ = clear_resources();
    set_state(PublisherControllerState::Idle);
    complete_stop_waiters(std::unexpected{control_issue(
        "publisher controller shut down during a stop operation")});
    command.completion.set_value();
    exit_requested_ = true;
}

void DefaultPublisherController::Impl::start_session(StartCommand& command) {
    if (state() != PublisherControllerState::Idle) {
        command.completion.set_value(std::unexpected{control_issue(
            "publisher can only start from the idle state")});
        return;
    }
    if (plan_.recovery_timeout <= std::chrono::milliseconds::zero() ||
        plan_.encoder.frame_rate.numerator == 0 ||
        plan_.encoder.frame_rate.denominator == 0) {
        command.completion.set_value(std::unexpected{control_issue(
            "publisher video session plan is invalid")});
        return;
    }

    set_state(PublisherControllerState::Starting);
    {
        std::lock_guard lock{mutex_};
        ++session_id_;
        last_issue_.reset();
        queued_failure_session_.reset();
        last_cleanup_ = {};
    }
    failure_recorded_for_session_ = false;
    capture_started_ = false;
    encoder_started_ = false;
    output_started_ = false;
    last_cleanup_ = clear_resources();

    const auto timeline =
        domain::SessionTimeline{domain::SessionTimeline::Clock::now()};

    auto output_started = output_worker_->start();
    if (!output_started) {
        fail_start(command, output_issue(
                                PublisherControllerOperation::StartOutput,
                                std::move(output_started.error())));
        return;
    }
    output_started_ = true;

    auto encoder_started = encoder_worker_->start(
        domain::VideoEncoderSessionConfig{plan_.encoder});
    if (!encoder_started) {
        fail_start(command, encoder_issue(
                                PublisherControllerOperation::StartEncoder,
                                std::move(encoder_started.error())));
        return;
    }
    encoder_started_ = true;

    auto capture_started = capture_worker_->start(
        domain::VideoCaptureSessionConfig{
            plan_.capture,
            timeline,
            plan_.encoder.frame_rate,
            plan_.recovery_timeout,
        });
    if (!capture_started) {
        fail_start(command, capture_issue(
                                PublisherControllerOperation::StartCapture,
                                std::move(capture_started.error())));
        return;
    }
    capture_started_ = true;

    {
        std::lock_guard lock{mutex_};
        ++started_sessions_;
    }
    set_state(PublisherControllerState::Running);
    command.completion.set_value(PublisherStarted{
        session_id_,
        timeline,
        std::move(*capture_started),
        std::move(*encoder_started),
        std::move(*output_started),
    });
}

void DefaultPublisherController::Impl::fail_start(
    StartCommand& command,
    PublisherControllerIssue issue) {
    abort_started_pipeline();
    last_cleanup_ = clear_resources();
    {
        std::lock_guard lock{mutex_};
        ++start_failures_;
        last_issue_ = issue;
    }
    set_state(PublisherControllerState::Idle);
    command.completion.set_value(std::unexpected{std::move(issue)});
}

void DefaultPublisherController::Impl::begin_normal_stop(
    StopCommand command) {
    stop_waiters_.push_back(std::move(command.completion));
    set_state(PublisherControllerState::Stopping);

    try {
        if (capture_started_) {
            capture_worker_->stop();
            capture_started_ = false;
        }
    } catch (const std::exception& error) {
        fail_running_session(internal_issue(
            PublisherControllerOperation::StopCapture, error.what()));
        return;
    } catch (...) {
        fail_running_session(internal_issue(
            PublisherControllerOperation::StopCapture,
            "video capture stop threw an unknown exception"));
        return;
    }

    launch_drain(DrainStage::Encoder);
}

void DefaultPublisherController::Impl::launch_drain(const DrainStage stage) {
    join_drain_thread();
    const auto current_session_id = session_id_;
    drain_thread_ = std::jthread{
        [this, current_session_id, stage](const std::stop_token) {
            DrainCompletedCommand completed{current_session_id, stage,
                                            std::nullopt};
            try {
                if (stage == DrainStage::Encoder) {
                    encoder_worker_->stop(domain::VideoEncoderStopMode::Drain);
                } else {
                    output_worker_->stop(domain::VideoOutputStopMode::Drain);
                }
            } catch (const std::exception& error) {
                completed.issue = internal_issue(
                    stage == DrainStage::Encoder
                        ? PublisherControllerOperation::DrainEncoder
                        : PublisherControllerOperation::DrainOutput,
                    error.what());
            } catch (...) {
                completed.issue = internal_issue(
                    stage == DrainStage::Encoder
                        ? PublisherControllerOperation::DrainEncoder
                        : PublisherControllerOperation::DrainOutput,
                    "worker drain threw an unknown exception");
            }
            enqueue_drain_completed(std::move(completed));
        }};
}

void DefaultPublisherController::Impl::finish_normal_stop() {
    last_cleanup_ = clear_resources();
    {
        std::lock_guard lock{mutex_};
        ++completed_sessions_;
    }
    set_state(PublisherControllerState::Idle);
    complete_stop_waiters(PublisherStopped{
        session_id_, last_cleanup_.frames, last_cleanup_.access_units});
}

void DefaultPublisherController::Impl::fail_running_session(
    PublisherControllerIssue issue) noexcept {
    if (failure_recorded_for_session_) {
        return;
    }
    failure_recorded_for_session_ = true;
    {
        std::lock_guard lock{mutex_};
        last_issue_ = issue;
    }
    set_state(PublisherControllerState::Stopping);

    abort_started_pipeline();
    join_drain_thread();
    last_cleanup_ = clear_resources();
    {
        std::lock_guard lock{mutex_};
        ++failed_sessions_;
    }
    set_state(PublisherControllerState::Failed);
    complete_stop_waiters(std::unexpected{std::move(issue)});
}

void DefaultPublisherController::Impl::abort_started_pipeline() noexcept {
    if (capture_started_) {
        try {
            capture_worker_->stop();
        } catch (...) {
        }
        capture_started_ = false;
    }
    if (encoder_started_) {
        try {
            encoder_worker_->stop(domain::VideoEncoderStopMode::Abort);
        } catch (...) {
        }
        encoder_started_ = false;
    }
    if (output_started_) {
        try {
            output_worker_->stop(domain::VideoOutputStopMode::Abort);
        } catch (...) {
        }
        output_started_ = false;
    }
}

DefaultPublisherController::Impl::ClearedResources
DefaultPublisherController::Impl::clear_resources() noexcept {
    const ClearedResources cleared{
        frame_store_->clear(),
        access_unit_queue_->clear(),
    };
    {
        std::lock_guard lock{mutex_};
        cleared_frames_ += cleared.frames;
        cleared_access_units_ += cleared.access_units;
    }
    return cleared;
}

void DefaultPublisherController::Impl::complete_stop_waiters(
    PublisherStopResult result) noexcept {
    for (auto& waiter : stop_waiters_) {
        try {
            waiter.set_value(result);
        } catch (...) {
        }
    }
    stop_waiters_.clear();
}

void DefaultPublisherController::Impl::join_drain_thread() noexcept {
    if (drain_thread_.joinable()) {
        drain_thread_.join();
    }
}

void DefaultPublisherController::Impl::subscribe_to_failures() {
    capture_subscription_ =
        notifier_->subscribe<domain::VideoCaptureWorkerFailed>(
            [this](const domain::VideoCaptureWorkerFailed& event) {
                std::lock_guard callback_lock{callback_mutex_};
                if (!failure_callbacks_enabled_) {
                    return;
                }
                enqueue_failure(capture_issue(
                    PublisherControllerOperation::VideoCaptureFailed,
                    event.issue));
            });
    encoder_subscription_ =
        notifier_->subscribe<domain::VideoEncoderWorkerFailed>(
            [this](const domain::VideoEncoderWorkerFailed& event) {
                std::lock_guard callback_lock{callback_mutex_};
                if (!failure_callbacks_enabled_) {
                    return;
                }
                enqueue_failure(encoder_issue(
                    PublisherControllerOperation::VideoEncoderFailed,
                    event.issue));
            });
    output_subscription_ =
        notifier_->subscribe<domain::VideoOutputWorkerFailed>(
            [this](const domain::VideoOutputWorkerFailed& event) {
                std::lock_guard callback_lock{callback_mutex_};
                if (!failure_callbacks_enabled_) {
                    return;
                }
                enqueue_failure(output_issue(
                    PublisherControllerOperation::VideoOutputFailed,
                    event.issue));
            });
}

void DefaultPublisherController::Impl::enqueue_failure(
    PublisherControllerIssue issue) noexcept {
    try {
        {
            std::lock_guard lock{mutex_};
            if (!accepting_commands_ ||
                (state_ != PublisherControllerState::Starting &&
                 state_ != PublisherControllerState::Running &&
                 state_ != PublisherControllerState::Stopping)) {
                return;
            }
            if (queued_failure_session_ == session_id_) {
                return;
            }
            queued_failure_session_ = session_id_;
            commands_.emplace_front(
                FailureCommand{session_id_, std::move(issue)});
        }
        command_cv_.notify_one();
    } catch (...) {
    }
}

void DefaultPublisherController::Impl::enqueue_drain_completed(
    DrainCompletedCommand command) noexcept {
    try {
        {
            std::lock_guard lock{mutex_};
            commands_.emplace_back(std::move(command));
        }
        command_cv_.notify_one();
    } catch (...) {
    }
}

void DefaultPublisherController::Impl::disable_failure_callbacks() noexcept {
    {
        std::lock_guard callback_lock{callback_mutex_};
        failure_callbacks_enabled_ = false;
    }
    if (capture_subscription_) {
        (void)capture_subscription_->unsubscribe();
    }
    if (encoder_subscription_) {
        (void)encoder_subscription_->unsubscribe();
    }
    if (output_subscription_) {
        (void)output_subscription_->unsubscribe();
    }
    capture_subscription_.reset();
    encoder_subscription_.reset();
    output_subscription_.reset();
}

void DefaultPublisherController::Impl::shutdown() noexcept {
    if (!controller_thread_.joinable()) {
        return;
    }

    ShutdownCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock{mutex_};
        accepting_commands_ = false;
        commands_.emplace_front(std::move(command));
    }
    command_cv_.notify_one();
    try {
        completion.get();
    } catch (...) {
    }
    controller_thread_.request_stop();
    command_cv_.notify_all();
    controller_thread_.join();
}

void DefaultPublisherController::Impl::cancel_pending_commands() noexcept {
    const auto cancelled = control_issue("publisher controller stopped");
    while (true) {
        std::optional<ControlCommand> command;
        {
            std::lock_guard lock{mutex_};
            if (commands_.empty()) {
                break;
            }
            command.emplace(std::move(commands_.front()));
            commands_.pop_front();
        }
        std::visit(
            [&cancelled](auto& value) {
                using Value = std::remove_cvref_t<decltype(value)>;
                try {
                    if constexpr (std::is_same_v<Value, StartCommand>) {
                        value.completion.set_value(std::unexpected{cancelled});
                    } else if constexpr (std::is_same_v<Value, StopCommand>) {
                        value.completion.set_value(std::unexpected{cancelled});
                    } else if constexpr (std::is_same_v<Value, ShutdownCommand>) {
                        value.completion.set_value();
                    }
                } catch (...) {
                }
            },
            *command);
    }
}

void DefaultPublisherController::Impl::set_state(
    const PublisherControllerState state) noexcept {
    {
        std::lock_guard lock{mutex_};
        state_ = state;
    }
    state_cv_.notify_all();
}

PublisherControllerIssue
DefaultPublisherController::Impl::current_worker_failure(
    const DrainStage stage) const {
    if (stage == DrainStage::Encoder) {
        const auto worker_stats = encoder_worker_->stats();
        if (worker_stats.last_issue) {
            return encoder_issue(PublisherControllerOperation::DrainEncoder,
                                 *worker_stats.last_issue);
        }
        return internal_issue(PublisherControllerOperation::DrainEncoder,
                              "video encoder did not return to idle after drain");
    }

    const auto worker_stats = output_worker_->stats();
    if (worker_stats.last_issue) {
        return output_issue(PublisherControllerOperation::DrainOutput,
                            *worker_stats.last_issue);
    }
    return internal_issue(PublisherControllerOperation::DrainOutput,
                          "video output did not return to idle after drain");
}

DefaultPublisherController::DefaultPublisherController(
    PublisherVideoSessionPlan plan,
    PublisherVideoPipeline pipeline,
    std::shared_ptr<contracts::Notifier> notifier)
    : impl_{std::make_unique<Impl>(std::move(plan), pipeline,
                                  std::move(notifier))} {}

DefaultPublisherController::~DefaultPublisherController() = default;

PublisherStartResult DefaultPublisherController::start_publishing() {
    return impl_->start_publishing();
}

PublisherStopResult DefaultPublisherController::stop_publishing() {
    return impl_->stop_publishing();
}

PublisherControllerState DefaultPublisherController::state() const noexcept {
    return impl_->state();
}

PublisherControllerStats DefaultPublisherController::stats() const noexcept {
    return impl_->stats();
}

PublisherWaitResult DefaultPublisherController::wait_for_terminal_for(
    const std::chrono::milliseconds timeout) {
    return impl_->wait_for_terminal_for(timeout);
}

}  // namespace semilive::publisher::application
