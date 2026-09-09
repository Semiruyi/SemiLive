#include <semilive/publisher/domain/worker/video_encoder_worker/default_video_encoder_worker.hpp>

#include <semilive/publisher/domain/resource/captured_video_frame_store/captured_video_frame_source.hpp>
#include <semilive/publisher/domain/resource/captured_video_frame_store/captured_video_frame_store_events.hpp>
#include <semilive/publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue_events.hpp>
#include <semilive/publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_sink.hpp>
#include <semilive/publisher/domain/worker/video_encoder_worker/video_encoder_worker_events.hpp>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace semilive::publisher::domain {
namespace {

VideoEncoderWorkerIssue worker_issue(const VideoEncoderWorkerOperation operation,
                                     std::string message) {
    return VideoEncoderWorkerIssue{operation, std::nullopt, std::move(message)};
}

VideoEncoderWorkerIssue encoder_issue(
    const VideoEncoderWorkerOperation operation,
    contracts::encoder::VideoEncoderIssue issue) {
    auto message = issue.message;
    return VideoEncoderWorkerIssue{
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

std::chrono::nanoseconds elapsed_since(
    const std::chrono::steady_clock::time_point started_at) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started_at);
}

}  // namespace

DefaultVideoEncoderWorker::DefaultVideoEncoderWorker(
    std::unique_ptr<contracts::encoder::VideoEncoderBackend> backend,
    CapturedVideoFrameSource& source,
    EncodedVideoAccessUnitSink& sink,
    std::shared_ptr<contracts::Notifier> notifier)
    : backend_(std::move(backend)),
      source_(&source),
      sink_(&sink),
      notifier_(std::move(notifier)) {
    if (!backend_ || !notifier_) {
        throw std::invalid_argument{
            "video encoder worker dependencies must not be null"};
    }

    subscribe_to_resources();
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

DefaultVideoEncoderWorker::~DefaultVideoEncoderWorker() {
    disable_notifications();
    shutdown_worker();
}

DefaultVideoEncoderWorker::StartResult DefaultVideoEncoderWorker::start(
    VideoEncoderSessionConfig config) {
    if (std::this_thread::get_id() == worker_.get_id()) {
        return std::unexpected{worker_issue(
            VideoEncoderWorkerOperation::Control,
            "video encoder worker cannot start itself")};
    }

    StartCommand command{std::move(config), {}};
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock{mutex_};
        if (module_state_ != ModuleState::Alive) {
            return std::unexpected{worker_issue(
                VideoEncoderWorkerOperation::Control,
                "video encoder worker thread is not available")};
        }
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

void DefaultVideoEncoderWorker::stop(const VideoEncoderStopMode mode) {
    if (std::this_thread::get_id() == worker_.get_id()) {
        throw std::logic_error{
            "video encoder worker cannot synchronously stop itself"};
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

VideoEncoderWorkerState DefaultVideoEncoderWorker::state() const noexcept {
    std::lock_guard lock{mutex_};
    return session_state_;
}

VideoEncoderWorkerStats DefaultVideoEncoderWorker::stats() const noexcept {
    std::lock_guard lock{mutex_};
    return stats_;
}

void DefaultVideoEncoderWorker::worker_main(
    const std::stop_token stop_token) noexcept {
    mark_module_alive();
    try {
        worker_loop(stop_token);
    } catch (const std::exception& error) {
        fail_session(worker_issue(VideoEncoderWorkerOperation::Internal,
                                  error.what()));
    } catch (...) {
        fail_session(worker_issue(
            VideoEncoderWorkerOperation::Internal,
            "unknown video encoder worker exception"));
    }

    cleanup_session(true);
    complete_drain_waiters();
    cancel_pending_commands();
    mark_module_stopped();
}

void DefaultVideoEncoderWorker::worker_loop(
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

std::optional<DefaultVideoEncoderWorker::ControlCommand>
DefaultVideoEncoderWorker::try_take_command() {
    std::lock_guard lock{mutex_};
    if (commands_.empty()) {
        return std::nullopt;
    }
    ControlCommand command = std::move(commands_.front());
    commands_.pop_front();
    return command;
}

void DefaultVideoEncoderWorker::wait_for_signal(
    const std::stop_token stop_token) {
    std::unique_lock lock{mutex_};
    cv_.wait(lock, [this, stop_token] {
        return stop_token.stop_requested() || !commands_.empty() ||
               frame_available_hint_ || queue_available_hint_;
    });
    frame_available_hint_ = false;
    queue_available_hint_ = false;
}

void DefaultVideoEncoderWorker::process_command(
    ControlCommand& command) noexcept {
    std::visit([this](auto& value) { process_command(value); }, command);
}

void DefaultVideoEncoderWorker::process_command(StartCommand& command) noexcept {
    if (!begin_start()) {
        complete(command.completion,
                 StartResult{std::unexpected{worker_issue(
                     VideoEncoderWorkerOperation::Control,
                     "video encoder worker is not idle")}});
        return;
    }

    auto result = start_session(std::move(command.config));
    finish_start(result.has_value());
    complete(command.completion, std::move(result));
}

void DefaultVideoEncoderWorker::process_command(StopCommand& command) noexcept {
    VideoEncoderWorkerState state;
    {
        std::lock_guard lock{mutex_};
        state = session_state_;
    }

    if (state == VideoEncoderWorkerState::Idle) {
        complete(command.completion);
        return;
    }

    if (command.mode == VideoEncoderStopMode::Drain &&
        (state == VideoEncoderWorkerState::Running ||
         state == VideoEncoderWorkerState::Draining)) {
        {
            std::lock_guard lock{mutex_};
            session_state_ = VideoEncoderWorkerState::Draining;
        }
        drain_waiters_.push_back(std::move(command.completion));
        return;
    }

    abort_session();
    complete(command.completion);
}

bool DefaultVideoEncoderWorker::begin_start() noexcept {
    {
        std::lock_guard lock{mutex_};
        if (session_state_ != VideoEncoderWorkerState::Idle) {
            return false;
        }
        session_state_ = VideoEncoderWorkerState::Starting;
        stats_ = {};
    }

    pending_access_units_.clear();
    previous_input_sequence_.reset();
    backpressure_started_at_.reset();
    backend_open_ = false;
    backend_flushed_ = false;
    return true;
}

DefaultVideoEncoderWorker::StartResult
DefaultVideoEncoderWorker::start_session(
    VideoEncoderSessionConfig config) noexcept {
    try {
        auto opened = backend_->open(config.encoder);
        if (!opened) {
            backend_->close();
            return std::unexpected{encoder_issue(
                VideoEncoderWorkerOperation::OpenBackend,
                std::move(opened.error()))};
        }

        backend_open_ = true;
        backend_flushed_ = false;
        record_started(*opened);
        return VideoEncoderStarted{std::move(*opened)};
    } catch (const std::exception& error) {
        backend_->close();
        backend_open_ = false;
        return std::unexpected{worker_issue(
            VideoEncoderWorkerOperation::OpenBackend, error.what())};
    } catch (...) {
        backend_->close();
        backend_open_ = false;
        return std::unexpected{worker_issue(
            VideoEncoderWorkerOperation::OpenBackend,
            "video encoder backend open threw an unknown exception")};
    }
}

void DefaultVideoEncoderWorker::finish_start(const bool succeeded) noexcept {
    std::lock_guard lock{mutex_};
    session_state_ = succeeded ? VideoEncoderWorkerState::Running
                               : VideoEncoderWorkerState::Idle;
}

bool DefaultVideoEncoderWorker::process_session_step() noexcept {
    try {
        return process_session_step_impl();
    } catch (const std::exception& error) {
        fail_session(worker_issue(VideoEncoderWorkerOperation::Internal,
                                  error.what()));
    } catch (...) {
        fail_session(worker_issue(
            VideoEncoderWorkerOperation::Internal,
            "unknown video encoder processing exception"));
    }
    return false;
}

bool DefaultVideoEncoderWorker::process_session_step_impl() {
    VideoEncoderWorkerState state;
    {
        std::lock_guard lock{mutex_};
        state = session_state_;
        if (!commands_.empty()) {
            return false;
        }
    }
    if (state != VideoEncoderWorkerState::Running &&
        state != VideoEncoderWorkerState::Draining) {
        return false;
    }

    if (!pending_access_units_.empty()) {
        return submit_pending_access_unit();
    }

    if (state == VideoEncoderWorkerState::Draining && backend_flushed_) {
        finish_drain();
        return true;
    }

    if (sink_->full()) {
        begin_backpressure();
        return false;
    }
    finish_backpressure();

    std::optional<model::CapturedVideoFrame> frame;
    try {
        frame = source_->try_pop();
    } catch (const std::exception& error) {
        fail_session(worker_issue(VideoEncoderWorkerOperation::ConsumeFrame,
                                  error.what()));
        return true;
    } catch (...) {
        fail_session(worker_issue(
            VideoEncoderWorkerOperation::ConsumeFrame,
            "captured video frame source threw an unknown exception"));
        return true;
    }
    if (frame) {
        return consume_and_encode_frame(std::move(*frame),
                                        state == VideoEncoderWorkerState::Draining);
    }

    if (state == VideoEncoderWorkerState::Draining) {
        return flush_backend();
    }
    return false;
}

bool DefaultVideoEncoderWorker::submit_pending_access_unit() {
    try {
        const auto result = sink_->try_push(
            std::move(pending_access_units_.front()));
        if (result == EncodedVideoAccessUnitPushResult::Full) {
            begin_backpressure();
            return false;
        }
    } catch (const std::exception& error) {
        fail_session(worker_issue(
            VideoEncoderWorkerOperation::PublishAccessUnit, error.what()));
        return true;
    } catch (...) {
        fail_session(worker_issue(
            VideoEncoderWorkerOperation::PublishAccessUnit,
            "encoded video access unit sink threw an unknown exception"));
        return true;
    }

    finish_backpressure();
    pending_access_units_.pop_front();
    record_submitted_access_unit();
    update_pending_count();
    return true;
}

bool DefaultVideoEncoderWorker::consume_and_encode_frame(
    model::CapturedVideoFrame frame,
    const bool draining) {
    record_consumed_frame(frame, draining);
    const auto captured_at = frame.captured_at;
    const auto started_at = Clock::now();

    contracts::encoder::VideoEncodeResult encoded;
    try {
        encoded = backend_->encode(frame);
    } catch (const std::exception& error) {
        fail_session(worker_issue(VideoEncoderWorkerOperation::Encode,
                                  error.what()));
        return true;
    } catch (...) {
        fail_session(worker_issue(
            VideoEncoderWorkerOperation::Encode,
            "video encoder backend encode threw an unknown exception"));
        return true;
    }
    const auto backend_time = Clock::now() - started_at;
    if (!encoded) {
        fail_session(encoder_issue(VideoEncoderWorkerOperation::Encode,
                                   std::move(encoded.error())));
        return true;
    }

    record_capture_to_encode(captured_at);
    append_batch(std::move(*encoded), backend_time, true, false);
    return true;
}

bool DefaultVideoEncoderWorker::flush_backend() {
    const auto started_at = Clock::now();
    contracts::encoder::VideoEncodeResult flushed;
    try {
        flushed = backend_->flush();
    } catch (const std::exception& error) {
        fail_session(worker_issue(VideoEncoderWorkerOperation::Flush,
                                  error.what()));
        return true;
    } catch (...) {
        fail_session(worker_issue(
            VideoEncoderWorkerOperation::Flush,
            "video encoder backend flush threw an unknown exception"));
        return true;
    }
    const auto backend_time = Clock::now() - started_at;
    if (!flushed) {
        fail_session(encoder_issue(VideoEncoderWorkerOperation::Flush,
                                   std::move(flushed.error())));
        return true;
    }

    backend_flushed_ = true;
    append_batch(std::move(*flushed), backend_time, false, true);
    return true;
}

void DefaultVideoEncoderWorker::append_batch(
    contracts::encoder::VideoEncodeBatch batch,
    const Clock::duration backend_time,
    const bool encode_call,
    const bool flush_batch) {
    record_backend_batch(batch, backend_time, encode_call, flush_batch);
    for (auto& access_unit : batch.access_units) {
        pending_access_units_.push_back(std::move(access_unit));
    }
    update_pending_count();
}

void DefaultVideoEncoderWorker::finish_drain() noexcept {
    cleanup_session(false);
    {
        std::lock_guard lock{mutex_};
        session_state_ = VideoEncoderWorkerState::Idle;
    }
    complete_drain_waiters();
}

void DefaultVideoEncoderWorker::abort_session() noexcept {
    cleanup_session(true);
    {
        std::lock_guard lock{mutex_};
        session_state_ = VideoEncoderWorkerState::Idle;
    }
    complete_drain_waiters();
}

void DefaultVideoEncoderWorker::cleanup_session(
    const bool count_abandoned_pending) noexcept {
    finish_backpressure();
    if (count_abandoned_pending && !pending_access_units_.empty()) {
        std::lock_guard lock{mutex_};
        stats_.aborted_pending_access_units += pending_access_units_.size();
    }
    pending_access_units_.clear();
    update_pending_count();
    if (backend_open_) {
        backend_->close();
        backend_open_ = false;
    }
    backend_flushed_ = false;
    previous_input_sequence_.reset();
}

void DefaultVideoEncoderWorker::fail_session(
    VideoEncoderWorkerIssue issue) noexcept {
    {
        std::lock_guard lock{mutex_};
        if (session_state_ != VideoEncoderWorkerState::Running &&
            session_state_ != VideoEncoderWorkerState::Draining) {
            return;
        }
    }

    cleanup_session(true);
    {
        std::lock_guard lock{mutex_};
        session_state_ = VideoEncoderWorkerState::Failed;
        ++stats_.fatal_failures;
        stats_.last_issue = issue;
    }
    complete_drain_waiters();
    notify_failure(issue);
}

void DefaultVideoEncoderWorker::notify_failure(
    const VideoEncoderWorkerIssue& issue) noexcept {
    try {
        (void)notifier_->send(VideoEncoderWorkerFailed{issue});
    } catch (...) {
    }
}

void DefaultVideoEncoderWorker::subscribe_to_resources() {
    frame_subscription_ = notifier_->subscribe<CapturedVideoFrameStoreNotEmpty>(
        [this](const CapturedVideoFrameStoreNotEmpty&) {
            signal_frame_available();
        });
    queue_subscription_ =
        notifier_->subscribe<EncodedVideoAccessUnitQueueNotFull>(
            [this](const EncodedVideoAccessUnitQueueNotFull&) {
                signal_queue_available();
            });
    if (!frame_subscription_ || !queue_subscription_) {
        throw std::runtime_error{
            "video encoder worker could not subscribe to resource events"};
    }
}

void DefaultVideoEncoderWorker::signal_frame_available() noexcept {
    std::lock_guard callback_lock{callback_mutex_};
    if (!callbacks_enabled_) {
        return;
    }
    {
        std::lock_guard lock{mutex_};
        frame_available_hint_ = true;
    }
    cv_.notify_one();
}

void DefaultVideoEncoderWorker::signal_queue_available() noexcept {
    std::lock_guard callback_lock{callback_mutex_};
    if (!callbacks_enabled_) {
        return;
    }
    {
        std::lock_guard lock{mutex_};
        queue_available_hint_ = true;
    }
    cv_.notify_one();
}

void DefaultVideoEncoderWorker::disable_notifications() noexcept {
    if (frame_subscription_) {
        (void)frame_subscription_->unsubscribe();
    }
    if (queue_subscription_) {
        (void)queue_subscription_->unsubscribe();
    }
    std::lock_guard lock{callback_mutex_};
    callbacks_enabled_ = false;
}

void DefaultVideoEncoderWorker::begin_backpressure() noexcept {
    if (backpressure_started_at_) {
        return;
    }
    backpressure_started_at_ = Clock::now();
    std::lock_guard lock{mutex_};
    ++stats_.access_unit_queue_full_events;
}

void DefaultVideoEncoderWorker::finish_backpressure() noexcept {
    if (!backpressure_started_at_) {
        return;
    }
    const auto elapsed = elapsed_since(*backpressure_started_at_);
    backpressure_started_at_.reset();
    std::lock_guard lock{mutex_};
    stats_.total_backpressure_time += elapsed;
    stats_.maximum_backpressure_time =
        std::max(stats_.maximum_backpressure_time, elapsed);
}

void DefaultVideoEncoderWorker::complete_drain_waiters() noexcept {
    while (!drain_waiters_.empty()) {
        complete(drain_waiters_.front());
        drain_waiters_.pop_front();
    }
}

void DefaultVideoEncoderWorker::shutdown_worker() noexcept {
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

void DefaultVideoEncoderWorker::cancel_pending_commands() noexcept {
    std::deque<ControlCommand> commands;
    {
        std::lock_guard lock{mutex_};
        commands.swap(commands_);
    }

    const auto cancelled = StartResult{std::unexpected{worker_issue(
        VideoEncoderWorkerOperation::ThreadInitialization,
        "video encoder worker is shutting down")}};
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

void DefaultVideoEncoderWorker::mark_module_alive() noexcept {
    {
        std::lock_guard lock{mutex_};
        module_state_ = ModuleState::Alive;
    }
    cv_.notify_all();
}

void DefaultVideoEncoderWorker::mark_module_stopped(
    std::string bootstrap_error) noexcept {
    {
        std::lock_guard lock{mutex_};
        bootstrap_error_ = std::move(bootstrap_error);
        module_state_ = ModuleState::Stopped;
    }
    cv_.notify_all();
}

void DefaultVideoEncoderWorker::record_started(
    const contracts::encoder::VideoEncoderInfo& info) {
    std::lock_guard lock{mutex_};
    stats_.encoder = info;
}

void DefaultVideoEncoderWorker::record_consumed_frame(
    const model::CapturedVideoFrame& frame,
    const bool draining) noexcept {
    std::lock_guard lock{mutex_};
    ++stats_.consumed_frames;
    stats_.drained_input_frames += draining ? 1U : 0U;
    if (previous_input_sequence_ && frame.sequence > *previous_input_sequence_ &&
        frame.sequence - *previous_input_sequence_ > 1U) {
        ++stats_.input_sequence_gaps;
        stats_.missing_input_frames +=
            frame.sequence - *previous_input_sequence_ - 1U;
    }
    previous_input_sequence_ = frame.sequence;
}

void DefaultVideoEncoderWorker::record_backend_batch(
    const contracts::encoder::VideoEncodeBatch& batch,
    const Clock::duration backend_time,
    const bool encode_call,
    const bool flush_batch) noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(backend_time);
    std::uint64_t key_frames = 0;
    std::uint64_t bytes = 0;
    for (const auto& access_unit : batch.access_units) {
        key_frames += access_unit.key_frame ? 1U : 0U;
        bytes += access_unit.annex_b.size();
    }

    std::lock_guard lock{mutex_};
    ++stats_.backend_calls;
    stats_.total_backend_time += elapsed;
    stats_.maximum_backend_time =
        std::max(stats_.maximum_backend_time, elapsed);
    stats_.total_preprocessing_time += batch.preprocessing_time;
    stats_.maximum_preprocessing_time =
        std::max(stats_.maximum_preprocessing_time, batch.preprocessing_time);
    stats_.total_codec_time += batch.codec_time;
    stats_.maximum_codec_time =
        std::max(stats_.maximum_codec_time, batch.codec_time);
    if (encode_call) {
        if (batch.access_units.empty()) {
            ++stats_.zero_output_calls;
        } else if (batch.access_units.size() == 1U) {
            ++stats_.single_output_calls;
        } else {
            ++stats_.multiple_output_calls;
        }
    }
    stats_.produced_access_units += batch.access_units.size();
    stats_.flushed_access_units += flush_batch ? batch.access_units.size() : 0U;
    stats_.key_frames += key_frames;
    stats_.encoded_bytes += bytes;
}

void DefaultVideoEncoderWorker::record_capture_to_encode(
    const Clock::time_point captured_at) noexcept {
    const auto now = Clock::now();
    if (captured_at > now) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - captured_at);
    std::lock_guard lock{mutex_};
    ++stats_.capture_to_encode_samples;
    stats_.total_capture_to_encode_time += elapsed;
    stats_.maximum_capture_to_encode_time =
        std::max(stats_.maximum_capture_to_encode_time, elapsed);
}

void DefaultVideoEncoderWorker::record_submitted_access_unit() noexcept {
    std::lock_guard lock{mutex_};
    ++stats_.submitted_access_units;
}

void DefaultVideoEncoderWorker::update_pending_count() noexcept {
    std::lock_guard lock{mutex_};
    stats_.pending_access_units = pending_access_units_.size();
}

}  // namespace semilive::publisher::domain
