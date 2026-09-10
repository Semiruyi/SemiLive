#include <semilive/publisher/domain/worker/video_output_worker/default_video_output_worker.hpp>

#include <semilive/publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue_events.hpp>
#include <semilive/publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_source.hpp>
#include <semilive/publisher/domain/worker/video_output_worker/video_output_worker_events.hpp>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace semilive::publisher::domain {
namespace {

VideoOutputWorkerIssue worker_issue(const VideoOutputWorkerOperation operation,
                                    std::string message) {
    return VideoOutputWorkerIssue{operation, std::nullopt, std::move(message)};
}

VideoOutputWorkerIssue output_issue(
    const VideoOutputWorkerOperation operation,
    contracts::output::VideoOutputIssue issue) {
    auto message = issue.message;
    return VideoOutputWorkerIssue{
        operation, std::move(issue), std::move(message)};
}

template <typename Value>
void complete(std::promise<Value>& promise, Value value) noexcept {
    try {
        promise.set_value(std::move(value));
    } catch (...) {
    }
}

void complete(std::promise<void>& promise) noexcept {
    try {
        promise.set_value();
    } catch (...) {
    }
}

}  // namespace

DefaultVideoOutputWorker::DefaultVideoOutputWorker(
    std::unique_ptr<contracts::output::VideoAccessUnitOutputBackend> backend,
    EncodedVideoAccessUnitSource& source,
    std::shared_ptr<contracts::Notifier> notifier)
    : backend_(std::move(backend)),
      source_(&source),
      notifier_(std::move(notifier)) {
    if (!backend_ || !notifier_) {
        throw std::invalid_argument{
            "video output worker dependencies must not be null"};
    }

    subscribe_to_resource();
    worker_ = std::jthread{[this](const std::stop_token stop_token) {
        worker_main(stop_token);
    }};

    std::unique_lock lock{mutex_};
    cv_.wait(lock, [this] { return module_state_ != ModuleState::Starting; });
    if (module_state_ == ModuleState::Stopped) {
        const auto message = bootstrap_error_;
        lock.unlock();
        worker_.join();
        throw std::runtime_error{message};
    }
}

DefaultVideoOutputWorker::~DefaultVideoOutputWorker() {
    disable_notifications();
    shutdown_worker();
}

DefaultVideoOutputWorker::StartResult DefaultVideoOutputWorker::start() {
    if (std::this_thread::get_id() == worker_.get_id()) {
        return std::unexpected{worker_issue(
            VideoOutputWorkerOperation::Control,
            "video output worker cannot start itself")};
    }

    StartCommand command;
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock{mutex_};
        if (module_state_ != ModuleState::Alive) {
            return std::unexpected{worker_issue(
                VideoOutputWorkerOperation::Control,
                "video output worker thread is not available")};
        }
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

void DefaultVideoOutputWorker::stop(const VideoOutputStopMode mode) {
    if (std::this_thread::get_id() == worker_.get_id()) {
        throw std::logic_error{
            "video output worker cannot synchronously stop itself"};
    }

    StopCommand command{mode, {}};
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock{mutex_};
        if (module_state_ != ModuleState::Alive) {
            return;
        }
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    completion.wait();
}

VideoOutputWorkerState DefaultVideoOutputWorker::state() const noexcept {
    std::lock_guard lock{mutex_};
    return session_state_;
}

VideoOutputWorkerStats DefaultVideoOutputWorker::stats() const noexcept {
    std::lock_guard lock{mutex_};
    return stats_;
}

void DefaultVideoOutputWorker::worker_main(
    const std::stop_token stop_token) noexcept {
    mark_module_alive();
    try {
        worker_loop(stop_token);
    } catch (const std::exception& error) {
        fail_session(worker_issue(VideoOutputWorkerOperation::Internal,
                                  error.what()));
    } catch (...) {
        fail_session(worker_issue(
            VideoOutputWorkerOperation::Internal,
            "unknown video output worker exception"));
    }

    cleanup_session();
    complete_drain_waiters();
    cancel_pending_commands();
    mark_module_stopped();
}

void DefaultVideoOutputWorker::worker_loop(
    const std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        if (auto command = try_take_command()) {
            process_command(*command);
            continue;
        }
        if (process_session_step()) {
            continue;
        }
        wait_for_signal(stop_token);
    }
}

std::optional<DefaultVideoOutputWorker::ControlCommand>
DefaultVideoOutputWorker::try_take_command() {
    std::lock_guard lock{mutex_};
    if (commands_.empty()) {
        return std::nullopt;
    }
    ControlCommand command = std::move(commands_.front());
    commands_.pop_front();
    return command;
}

void DefaultVideoOutputWorker::wait_for_signal(
    const std::stop_token stop_token) {
    std::unique_lock lock{mutex_};
    cv_.wait(lock, [this, stop_token] {
        return stop_token.stop_requested() || !commands_.empty() ||
               access_unit_available_hint_;
    });
    access_unit_available_hint_ = false;
}

void DefaultVideoOutputWorker::process_command(
    ControlCommand& command) noexcept {
    std::visit([this](auto& value) { process_command(value); }, command);
}

void DefaultVideoOutputWorker::process_command(
    StartCommand& command) noexcept {
    if (!begin_start()) {
        complete(command.completion,
                 StartResult{std::unexpected{worker_issue(
                     VideoOutputWorkerOperation::Control,
                     "video output worker is not idle")}});
        return;
    }

    auto result = start_session();
    finish_start(result.has_value());
    complete(command.completion, std::move(result));
}

void DefaultVideoOutputWorker::process_command(
    StopCommand& command) noexcept {
    VideoOutputWorkerState state;
    {
        std::lock_guard lock{mutex_};
        state = session_state_;
    }

    if (state == VideoOutputWorkerState::Idle) {
        complete(command.completion);
        return;
    }

    if (command.mode == VideoOutputStopMode::Drain &&
        (state == VideoOutputWorkerState::Running ||
         state == VideoOutputWorkerState::Draining)) {
        {
            std::lock_guard lock{mutex_};
            session_state_ = VideoOutputWorkerState::Draining;
        }
        drain_waiters_.push_back(std::move(command.completion));
        return;
    }

    abort_session();
    complete(command.completion);
}

bool DefaultVideoOutputWorker::begin_start() noexcept {
    {
        std::lock_guard lock{mutex_};
        if (session_state_ != VideoOutputWorkerState::Idle) {
            return false;
        }
        session_state_ = VideoOutputWorkerState::Starting;
        stats_ = {};
    }

    previous_presentation_time_.reset();
    previous_source_sequence_.reset();
    backend_open_ = false;
    backend_flushed_ = false;
    return true;
}

DefaultVideoOutputWorker::StartResult
DefaultVideoOutputWorker::start_session() noexcept {
    try {
        auto opened = backend_->open();
        if (!opened) {
            backend_->close();
            return std::unexpected{output_issue(
                VideoOutputWorkerOperation::OpenBackend,
                std::move(opened.error()))};
        }

        backend_open_ = true;
        backend_flushed_ = false;
        record_started(*opened);
        return VideoOutputStarted{std::move(*opened)};
    } catch (const std::exception& error) {
        backend_->close();
        backend_open_ = false;
        return std::unexpected{worker_issue(
            VideoOutputWorkerOperation::OpenBackend, error.what())};
    } catch (...) {
        backend_->close();
        backend_open_ = false;
        return std::unexpected{worker_issue(
            VideoOutputWorkerOperation::OpenBackend,
            "video output backend open threw an unknown exception")};
    }
}

void DefaultVideoOutputWorker::finish_start(const bool succeeded) noexcept {
    std::lock_guard lock{mutex_};
    session_state_ = succeeded ? VideoOutputWorkerState::Running
                               : VideoOutputWorkerState::Idle;
}

bool DefaultVideoOutputWorker::process_session_step() noexcept {
    try {
        return process_session_step_impl();
    } catch (const std::exception& error) {
        fail_session(worker_issue(VideoOutputWorkerOperation::Internal,
                                  error.what()));
    } catch (...) {
        fail_session(worker_issue(
            VideoOutputWorkerOperation::Internal,
            "unknown video output processing exception"));
    }
    return false;
}

bool DefaultVideoOutputWorker::process_session_step_impl() {
    VideoOutputWorkerState state;
    {
        std::lock_guard lock{mutex_};
        state = session_state_;
        if (!commands_.empty()) {
            return false;
        }
    }
    if (state != VideoOutputWorkerState::Running &&
        state != VideoOutputWorkerState::Draining) {
        return false;
    }

    std::optional<model::EncodedVideoAccessUnit> access_unit;
    try {
        access_unit = source_->try_pop();
    } catch (const std::exception& error) {
        fail_session(worker_issue(
            VideoOutputWorkerOperation::ConsumeAccessUnit, error.what()));
        return true;
    } catch (...) {
        fail_session(worker_issue(
            VideoOutputWorkerOperation::ConsumeAccessUnit,
            "encoded video access unit source threw an unknown exception"));
        return true;
    }
    if (access_unit) {
        return consume_access_unit(std::move(*access_unit),
                                   state == VideoOutputWorkerState::Draining);
    }

    if (state == VideoOutputWorkerState::Draining) {
        if (!backend_flushed_) {
            return flush_backend();
        }
        finish_drain();
        return true;
    }
    return false;
}

bool DefaultVideoOutputWorker::consume_access_unit(
    model::EncodedVideoAccessUnit access_unit,
    const bool draining) {
    if (auto validation_error = validate_access_unit(access_unit)) {
        fail_session(std::move(*validation_error));
        return true;
    }

    record_consumed_access_unit(access_unit, draining);
    previous_presentation_time_ = access_unit.presentation_time;
    previous_source_sequence_ = access_unit.source_sequence;

    const auto captured_at = access_unit.captured_at;
    const auto started_at = Clock::now();
    contracts::output::VideoOutputConsumeResult consumed;
    try {
        consumed = backend_->consume(access_unit);
    } catch (const std::exception& error) {
        fail_session(worker_issue(VideoOutputWorkerOperation::Output,
                                  error.what()));
        return true;
    } catch (...) {
        fail_session(worker_issue(
            VideoOutputWorkerOperation::Output,
            "video output backend consume threw an unknown exception"));
        return true;
    }
    const auto backend_time = Clock::now() - started_at;
    if (!consumed) {
        fail_session(output_issue(VideoOutputWorkerOperation::Output,
                                  std::move(consumed.error())));
        return true;
    }

    record_backend_result(*consumed, backend_time, false);
    record_capture_to_output(captured_at);
    return true;
}

std::optional<VideoOutputWorkerIssue>
DefaultVideoOutputWorker::validate_access_unit(
    const model::EncodedVideoAccessUnit& access_unit) const {
    if (access_unit.annex_b.empty()) {
        return worker_issue(VideoOutputWorkerOperation::ValidateAccessUnit,
                            "encoded video access unit must not be empty");
    }
    if (access_unit.presentation_time < model::MediaTime::zero()) {
        return worker_issue(
            VideoOutputWorkerOperation::ValidateAccessUnit,
            "encoded video access unit presentation time must not be negative");
    }
    if (previous_presentation_time_ &&
        access_unit.presentation_time <= *previous_presentation_time_) {
        return worker_issue(
            VideoOutputWorkerOperation::ValidateAccessUnit,
            "encoded video access unit presentation time must be strictly increasing");
    }
    if (previous_source_sequence_ &&
        access_unit.source_sequence <= *previous_source_sequence_) {
        return worker_issue(
            VideoOutputWorkerOperation::ValidateAccessUnit,
            "encoded video access unit source sequence must be strictly increasing");
    }
    return std::nullopt;
}

bool DefaultVideoOutputWorker::flush_backend() {
    const auto started_at = Clock::now();
    contracts::output::VideoOutputFlushResult flushed;
    try {
        flushed = backend_->flush();
    } catch (const std::exception& error) {
        fail_session(worker_issue(VideoOutputWorkerOperation::Flush,
                                  error.what()));
        return true;
    } catch (...) {
        fail_session(worker_issue(
            VideoOutputWorkerOperation::Flush,
            "video output backend flush threw an unknown exception"));
        return true;
    }
    const auto backend_time = Clock::now() - started_at;
    if (!flushed) {
        fail_session(output_issue(VideoOutputWorkerOperation::Flush,
                                  std::move(flushed.error())));
        return true;
    }

    backend_flushed_ = true;
    record_backend_result(*flushed, backend_time, true);
    return true;
}

void DefaultVideoOutputWorker::finish_drain() noexcept {
    cleanup_session();
    {
        std::lock_guard lock{mutex_};
        session_state_ = VideoOutputWorkerState::Idle;
    }
    complete_drain_waiters();
}

void DefaultVideoOutputWorker::abort_session() noexcept {
    cleanup_session();
    {
        std::lock_guard lock{mutex_};
        session_state_ = VideoOutputWorkerState::Idle;
    }
    complete_drain_waiters();
}

void DefaultVideoOutputWorker::cleanup_session() noexcept {
    if (backend_open_) {
        backend_->close();
        backend_open_ = false;
    }
    backend_flushed_ = false;
    previous_presentation_time_.reset();
    previous_source_sequence_.reset();
}

void DefaultVideoOutputWorker::fail_session(
    VideoOutputWorkerIssue issue) noexcept {
    {
        std::lock_guard lock{mutex_};
        if (session_state_ != VideoOutputWorkerState::Running &&
            session_state_ != VideoOutputWorkerState::Draining) {
            return;
        }
    }

    cleanup_session();
    {
        std::lock_guard lock{mutex_};
        session_state_ = VideoOutputWorkerState::Failed;
        ++stats_.fatal_failures;
        stats_.last_issue = issue;
    }
    complete_drain_waiters();
    notify_failure(issue);
}

void DefaultVideoOutputWorker::notify_failure(
    const VideoOutputWorkerIssue& issue) noexcept {
    try {
        (void)notifier_->send(VideoOutputWorkerFailed{issue});
    } catch (...) {
    }
}

void DefaultVideoOutputWorker::subscribe_to_resource() {
    source_subscription_ =
        notifier_->subscribe<EncodedVideoAccessUnitQueueNotEmpty>(
            [this](const EncodedVideoAccessUnitQueueNotEmpty&) {
                signal_access_unit_available();
            });
    if (!source_subscription_) {
        throw std::runtime_error{
            "video output worker could not subscribe to resource events"};
    }
}

void DefaultVideoOutputWorker::signal_access_unit_available() noexcept {
    std::lock_guard callback_lock{callback_mutex_};
    if (!callbacks_enabled_) {
        return;
    }
    {
        std::lock_guard lock{mutex_};
        access_unit_available_hint_ = true;
    }
    cv_.notify_one();
}

void DefaultVideoOutputWorker::disable_notifications() noexcept {
    if (source_subscription_) {
        (void)source_subscription_->unsubscribe();
    }
    std::lock_guard lock{callback_mutex_};
    callbacks_enabled_ = false;
}

void DefaultVideoOutputWorker::complete_drain_waiters() noexcept {
    while (!drain_waiters_.empty()) {
        complete(drain_waiters_.front());
        drain_waiters_.pop_front();
    }
}

void DefaultVideoOutputWorker::shutdown_worker() noexcept {
    {
        std::lock_guard lock{mutex_};
        if (module_state_ == ModuleState::Stopped) {
            return;
        }
        module_state_ = ModuleState::Stopping;
    }
    worker_.request_stop();
    cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void DefaultVideoOutputWorker::cancel_pending_commands() noexcept {
    std::deque<ControlCommand> commands;
    {
        std::lock_guard lock{mutex_};
        commands.swap(commands_);
    }

    const auto cancelled = StartResult{std::unexpected{worker_issue(
        VideoOutputWorkerOperation::ThreadInitialization,
        "video output worker is shutting down")}};
    for (auto& command : commands) {
        std::visit(
            [&cancelled](auto& value) {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, StartCommand>) {
                    complete(value.completion, cancelled);
                } else {
                    complete(value.completion);
                }
            },
            command);
    }
}

void DefaultVideoOutputWorker::mark_module_alive() noexcept {
    {
        std::lock_guard lock{mutex_};
        module_state_ = ModuleState::Alive;
    }
    cv_.notify_all();
}

void DefaultVideoOutputWorker::mark_module_stopped(
    std::string bootstrap_error) noexcept {
    {
        std::lock_guard lock{mutex_};
        bootstrap_error_ = std::move(bootstrap_error);
        module_state_ = ModuleState::Stopped;
    }
    cv_.notify_all();
}

void DefaultVideoOutputWorker::record_started(
    const contracts::output::VideoOutputInfo& info) {
    std::lock_guard lock{mutex_};
    stats_.output = info;
}

void DefaultVideoOutputWorker::record_consumed_access_unit(
    const model::EncodedVideoAccessUnit& access_unit,
    const bool draining) noexcept {
    std::lock_guard lock{mutex_};
    ++stats_.consumed_access_units;
    stats_.drained_access_units += draining ? 1U : 0U;
    stats_.key_frames += access_unit.key_frame ? 1U : 0U;
    stats_.input_bytes += access_unit.annex_b.size();
}

void DefaultVideoOutputWorker::record_backend_result(
    const contracts::output::VideoOutputReceipt& receipt,
    const Clock::duration backend_time,
    const bool flush_call) noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(backend_time);
    std::lock_guard lock{mutex_};
    ++stats_.backend_calls;
    stats_.flush_calls += flush_call ? 1U : 0U;
    stats_.total_backend_time += elapsed;
    stats_.maximum_backend_time =
        std::max(stats_.maximum_backend_time, elapsed);
    stats_.emitted_units += receipt.emitted_units;
    stats_.emitted_bytes += receipt.emitted_bytes;
}

void DefaultVideoOutputWorker::record_capture_to_output(
    const Clock::time_point captured_at) noexcept {
    const auto now = Clock::now();
    if (captured_at > now) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - captured_at);
    std::lock_guard lock{mutex_};
    ++stats_.capture_to_output_samples;
    stats_.total_capture_to_output_time += elapsed;
    stats_.maximum_capture_to_output_time =
        std::max(stats_.maximum_capture_to_output_time, elapsed);
}

}  // namespace semilive::publisher::domain
