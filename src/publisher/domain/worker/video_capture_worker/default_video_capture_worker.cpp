#include "publisher/domain/worker/video_capture_worker/default_video_capture_worker.hpp"

#include "publisher/domain/worker/video_capture_worker/video_capture_worker_events.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#include <objbase.h>
#endif

namespace semilive::publisher::domain {
namespace {

using contracts::capture::DesktopCaptureIssue;
using contracts::capture::DesktopCaptureObservation;
using contracts::capture::DesktopImage;
using contracts::capture::DesktopNoChange;
using contracts::capture::DesktopTemporarilyUnavailable;

VideoCaptureWorkerIssue worker_issue(const VideoCaptureWorkerOperation operation,
                                     std::string message) {
    return VideoCaptureWorkerIssue{operation, std::nullopt, std::move(message)};
}

VideoCaptureWorkerIssue capture_issue(const VideoCaptureWorkerOperation operation,
                                      DesktopCaptureIssue issue) {
    auto message = issue.message;
    return VideoCaptureWorkerIssue{operation, std::move(issue), std::move(message)};
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

class ThreadApartment final {
public:
    [[nodiscard]] std::optional<std::int64_t> initialize() noexcept {
#ifdef _WIN32
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result)) {
            return static_cast<std::int64_t>(result);
        }
        initialized_ = true;
#endif
        return std::nullopt;
    }

    ~ThreadApartment() {
#ifdef _WIN32
        if (initialized_) {
            CoUninitialize();
        }
#endif
    }

private:
    bool initialized_ = false;
};

bool valid_image_layout(const DesktopImage& image) noexcept {
    if (image.width == 0 || image.height == 0 ||
        image.width > std::numeric_limits<std::uint32_t>::max() / 4U) {
        return false;
    }
    const auto stride = image.width * 4U;
    const auto size = static_cast<std::uint64_t>(stride) * image.height;
    return image.stride == stride && size <= std::numeric_limits<std::size_t>::max() &&
           image.bgra.size() == static_cast<std::size_t>(size);
}

}  // namespace

DefaultVideoCaptureWorker::DefaultVideoCaptureWorker(
    std::unique_ptr<contracts::capture::DesktopCaptureBackend> backend,
    CapturedVideoFrameSink& sink,
    std::shared_ptr<infra::Notifier> notifier)
    : backend_(std::move(backend)), sink_(&sink), notifier_(std::move(notifier)) {
    if (!backend_ || !notifier_) {
        throw std::invalid_argument{"video capture worker dependencies must not be null"};
    }

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

DefaultVideoCaptureWorker::~DefaultVideoCaptureWorker() {
    shutdown_worker();
}

DefaultVideoCaptureWorker::StartResult
DefaultVideoCaptureWorker::start(VideoCaptureSessionConfig config) {
    if (std::this_thread::get_id() == worker_.get_id()) {
        return std::unexpected{
            worker_issue(VideoCaptureWorkerOperation::Control,
                         "video capture worker cannot start itself")};
    }

    StartCommand command{std::move(config), {}};
    auto completion = command.completion.get_future();
    {
        std::lock_guard lock{mutex_};
        if (module_state_ != ModuleState::Alive) {
            return std::unexpected{worker_issue(
                VideoCaptureWorkerOperation::Control,
                "video capture worker thread is not available")};
        }
        commands_.emplace_back(std::move(command));
    }
    cv_.notify_one();
    return completion.get();
}

void DefaultVideoCaptureWorker::stop() {
    if (std::this_thread::get_id() == worker_.get_id()) {
        throw std::logic_error{"video capture worker cannot synchronously stop itself"};
    }

    StopCommand command;
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

VideoCaptureWorkerState DefaultVideoCaptureWorker::state() const noexcept {
    std::lock_guard lock{mutex_};
    return session_state_;
}

VideoCaptureWorkerStats DefaultVideoCaptureWorker::stats() const noexcept {
    std::lock_guard lock{mutex_};
    return stats_;
}

void DefaultVideoCaptureWorker::worker_main(const std::stop_token stop_token) noexcept {
    ThreadApartment apartment;
    if (const auto error = apartment.initialize()) {
        mark_module_stopped("failed to initialize capture thread COM apartment: " +
                            std::to_string(*error));
        return;
    }

    mark_module_alive();
    try {
        worker_loop(stop_token);
    } catch (const std::exception& error) {
        fail_running(worker_issue(VideoCaptureWorkerOperation::Internal, error.what()));
    } catch (...) {
        fail_running(worker_issue(VideoCaptureWorkerOperation::Internal,
                                  "unknown video capture worker exception"));
    }
    cleanup_session();
    cancel_pending_commands();
    mark_module_stopped();
}

void DefaultVideoCaptureWorker::worker_loop(const std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        auto command = wait_for_work(stop_token);
        if (stop_token.stop_requested()) {
            return;
        }
        if (command) {
            process_command(*command);
        } else {
            process_capture_tick();
        }
    }
}

std::optional<DefaultVideoCaptureWorker::ControlCommand>
DefaultVideoCaptureWorker::wait_for_work(const std::stop_token stop_token) {
    std::unique_lock lock{mutex_};
    const auto interrupted = [this, stop_token] {
        return stop_token.stop_requested() || !commands_.empty();
    };
    if (session_state_ == VideoCaptureWorkerState::Running) {
        if (!next_tick_) {
            throw std::logic_error{"running capture worker has no next frame tick"};
        }
        (void)cv_.wait_until(lock, next_tick_->deadline, interrupted); // 此处粗睡眠导致帧间隔不稳定
    } else {
        cv_.wait(lock, interrupted);
    }

    if (stop_token.stop_requested() || commands_.empty()) {
        return std::nullopt;
    }
    ControlCommand command = std::move(commands_.front());
    commands_.pop_front();
    return command;
}

void DefaultVideoCaptureWorker::process_command(ControlCommand& command) noexcept {
    std::visit([this](auto& value) { process_command(value); }, command);
}

void DefaultVideoCaptureWorker::process_command(StartCommand& command) noexcept {
    if (!begin_start()) {
        complete(command.completion,
                 StartResult{std::unexpected{worker_issue(
                     VideoCaptureWorkerOperation::Control,
                     "video capture worker is not idle")}});
        return;
    }

    auto result = start_session(std::move(command.config));
    finish_start(result.has_value());
    complete(command.completion, std::move(result));
}

void DefaultVideoCaptureWorker::process_command(StopCommand& command) noexcept {
    {
        std::lock_guard lock{mutex_};
        if (session_state_ == VideoCaptureWorkerState::Idle) {
            complete(command.completion);
            return;
        }
        session_state_ = VideoCaptureWorkerState::Stopping;
    }

    cleanup_session();
    {
        std::lock_guard lock{mutex_};
        session_state_ = VideoCaptureWorkerState::Idle;
        last_issue_.reset();
    }
    complete(command.completion);
}

bool DefaultVideoCaptureWorker::begin_start() noexcept {
    std::lock_guard lock{mutex_};
    if (session_state_ != VideoCaptureWorkerState::Idle) {
        return false;
    }
    session_state_ = VideoCaptureWorkerState::Starting;
    stats_ = {};
    last_issue_.reset();
    return true;
}

DefaultVideoCaptureWorker::StartResult
DefaultVideoCaptureWorker::start_session(VideoCaptureSessionConfig config) noexcept {
    try {
        auto result = start_session_impl(std::move(config));
        if (!result) {
            backend_->close();
            backend_open_ = false;
            cleanup_session();
        }
        return result;
    } catch (const std::exception& error) {
        backend_->close();
        backend_open_ = false;
        cleanup_session();
        return std::unexpected{
            worker_issue(VideoCaptureWorkerOperation::Internal, error.what())};
    } catch (...) {
        backend_->close();
        backend_open_ = false;
        cleanup_session();
        return std::unexpected{worker_issue(
            VideoCaptureWorkerOperation::Internal,
            "unknown exception while starting video capture")};
    }
}

DefaultVideoCaptureWorker::StartResult
DefaultVideoCaptureWorker::start_session_impl(VideoCaptureSessionConfig config) {
    if (config.recovery_timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected{worker_issue(
            VideoCaptureWorkerOperation::Configure,
            "video capture recovery timeout must be positive")};
    }

    auto opened = open_backend(config.capture);
    if (!opened) {
        return std::unexpected{std::move(opened.error())};
    }
    backend_open_ = true;
    const auto track_start = Clock::now();
    try {
        scheduler_.emplace(config.timeline, config.frame_rate, track_start);
        next_tick_ = scheduler_->initial_tick();
    } catch (const std::exception& error) {
        return std::unexpected{
            worker_issue(VideoCaptureWorkerOperation::Schedule, error.what())};
    }

    recovery_timeout_ = config.recovery_timeout;
    next_sequence_ = 0;
    reset_session_stats(*opened);
    return VideoCaptureStarted{std::move(*opened), track_start,
                               next_tick_->presentation_time};
}

std::expected<contracts::capture::DesktopCaptureInfo, VideoCaptureWorkerIssue>
DefaultVideoCaptureWorker::open_backend(
    const contracts::capture::DesktopCaptureConfig& config) noexcept {
    try {
        auto opened = backend_->open(config);
        if (!opened) {
            return std::unexpected{
                capture_issue(VideoCaptureWorkerOperation::OpenBackend,
                              std::move(opened.error()))};
        }
        return std::move(*opened);
    } catch (const std::exception& error) {
        return std::unexpected{
            worker_issue(VideoCaptureWorkerOperation::OpenBackend, error.what())};
    } catch (...) {
        return std::unexpected{worker_issue(
            VideoCaptureWorkerOperation::OpenBackend,
            "desktop capture backend open threw an unknown exception")};
    }
}

void DefaultVideoCaptureWorker::finish_start(const bool succeeded) noexcept {
    std::lock_guard lock{mutex_};
    session_state_ = succeeded ? VideoCaptureWorkerState::Running
                               : VideoCaptureWorkerState::Idle;
}

void DefaultVideoCaptureWorker::cleanup_session() noexcept {
    if (backend_open_) {
        backend_->close();
        backend_open_ = false;
    }
    scheduler_.reset();
    next_tick_.reset();
    latest_image_.reset();
    recovering_since_.reset();
    last_recovery_issue_.reset();
    next_sequence_ = 0;
}

void DefaultVideoCaptureWorker::process_capture_tick() noexcept {
    try {
        auto result = process_capture_tick_impl();
        if (!result) {
            fail_running(std::move(result.error()));
        }
    } catch (const std::exception& error) {
        fail_running(worker_issue(VideoCaptureWorkerOperation::Internal, error.what()));
    } catch (...) {
        fail_running(worker_issue(VideoCaptureWorkerOperation::Internal,
                                  "unknown video capture processing exception"));
    }
}

std::expected<void, VideoCaptureWorkerIssue>
DefaultVideoCaptureWorker::process_capture_tick_impl() {
    auto observation = capture_once();
    if (!observation) {
        return std::unexpected{std::move(observation.error())};
    }
    record_processed_tick();

    auto content = prepare_frame(std::move(*observation));
    if (!content) {
        return std::unexpected{std::move(content.error())};
    }
    if (control_command_pending()) {
        return {};
    }
    if (*content == FrameContent::Missing) {
        record_missing_frame();
    } else if (auto published = publish_frame(*content); !published) {
        return std::unexpected{std::move(published.error())};
    }
    return advance_schedule();
}

DefaultVideoCaptureWorker::ObservationResult
DefaultVideoCaptureWorker::capture_once() noexcept {
    const auto started_at = Clock::now();
    try {
        auto result = backend_->capture_latest();
        record_capture_time(Clock::now() - started_at);
        if (!result) {
            return std::unexpected{
                capture_issue(VideoCaptureWorkerOperation::Capture,
                              std::move(result.error()))};
        }
        return std::move(*result);
    } catch (const std::exception& error) {
        record_capture_time(Clock::now() - started_at);
        return std::unexpected{
            worker_issue(VideoCaptureWorkerOperation::Capture, error.what())};
    } catch (...) {
        record_capture_time(Clock::now() - started_at);
        return std::unexpected{worker_issue(
            VideoCaptureWorkerOperation::Capture,
            "desktop capture backend threw an unknown exception")};
    }
}

std::expected<DefaultVideoCaptureWorker::FrameContent, VideoCaptureWorkerIssue>
DefaultVideoCaptureWorker::prepare_frame(DesktopCaptureObservation observation) {
    return std::visit(
        [this](auto&& value)
            -> std::expected<FrameContent, VideoCaptureWorkerIssue> {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, DesktopImage>) {
                return adopt_image(std::move(value));
            } else if constexpr (std::is_same_v<Value, DesktopNoChange>) {
                return observe_no_change();
            } else {
                return observe_temporary(std::move(value));
            }
        },
        std::move(observation));
}

std::expected<DefaultVideoCaptureWorker::FrameContent, VideoCaptureWorkerIssue>
DefaultVideoCaptureWorker::adopt_image(DesktopImage image) {
    if (!valid_image_layout(image)) {
        return std::unexpected{worker_issue(
            VideoCaptureWorkerOperation::Capture,
            "desktop capture backend returned an invalid BGRA image")};
    }

    const bool recovered = recovering_since_.has_value();
    auto shared_image = std::make_shared<const BgraFrameBuffer>(BgraFrameBuffer{
        std::move(image.bgra), image.width, image.height, image.stride});
    latest_image_ = std::move(shared_image);
    recovering_since_.reset();
    last_recovery_issue_.reset();
    record_image(*latest_image_, recovered);
    return FrameContent::NewImage;
}

DefaultVideoCaptureWorker::FrameContent
DefaultVideoCaptureWorker::observe_no_change() noexcept {
    const bool recovered = recovering_since_.has_value();
    recovering_since_.reset();
    last_recovery_issue_.reset();
    record_no_change(recovered);
    return latest_image_ ? FrameContent::Repeated : FrameContent::Missing;
}

std::expected<DefaultVideoCaptureWorker::FrameContent, VideoCaptureWorkerIssue>
DefaultVideoCaptureWorker::observe_temporary(
    DesktopTemporarilyUnavailable unavailable) {
    const auto now = Clock::now();
    const bool new_episode = !recovering_since_;
    if (new_episode) {
        recovering_since_ = now;
    }
    last_recovery_issue_ = std::move(unavailable.issue);
    record_temporary(new_episode);

    if (now - *recovering_since_ >= recovery_timeout_) {
        return std::unexpected{
            capture_issue(VideoCaptureWorkerOperation::Recovery,
                          *last_recovery_issue_)};
    }
    return latest_image_ ? FrameContent::Repeated : FrameContent::Missing;
}

std::expected<void, VideoCaptureWorkerIssue>
DefaultVideoCaptureWorker::publish_frame(const FrameContent content) noexcept {
    if (!latest_image_ || !next_tick_) {
        return std::unexpected{worker_issue(
            VideoCaptureWorkerOperation::Internal,
            "video capture frame state is incomplete")};
    }

    CapturedVideoFrame frame{latest_image_, next_sequence_,
                             next_tick_->presentation_time, Clock::now()};
    try {
        const auto pushed = sink_->try_push(std::move(frame));
        ++next_sequence_;
        record_published(content, pushed);
        return {};
    } catch (const std::exception& error) {
        return std::unexpected{
            worker_issue(VideoCaptureWorkerOperation::Publish, error.what())};
    } catch (...) {
        return std::unexpected{worker_issue(
            VideoCaptureWorkerOperation::Publish,
            "captured video frame sink threw an unknown exception")};
    }
}

std::expected<void, VideoCaptureWorkerIssue>
DefaultVideoCaptureWorker::advance_schedule() noexcept {
    try {
        const auto schedule = scheduler_->advance_after(Clock::now());
        next_tick_ = schedule.tick;
        record_schedule(schedule.skipped_ticks);
        return {};
    } catch (const std::exception& error) {
        return std::unexpected{
            worker_issue(VideoCaptureWorkerOperation::Schedule, error.what())};
    } catch (...) {
        return std::unexpected{worker_issue(
            VideoCaptureWorkerOperation::Schedule,
            "frame scheduler threw an unknown exception")};
    }
}

bool DefaultVideoCaptureWorker::control_command_pending() const noexcept {
    std::lock_guard lock{mutex_};
    return !commands_.empty() || module_state_ != ModuleState::Alive;
}

void DefaultVideoCaptureWorker::fail_running(VideoCaptureWorkerIssue issue) noexcept {
    {
        std::lock_guard lock{mutex_};
        if (session_state_ != VideoCaptureWorkerState::Running) {
            return;
        }
    }
    cleanup_session();
    record_fatal_failure();
    {
        std::lock_guard lock{mutex_};
        session_state_ = VideoCaptureWorkerState::Failed;
        last_issue_ = issue;
    }
    notify_failure(issue);
}

void DefaultVideoCaptureWorker::notify_failure(
    const VideoCaptureWorkerIssue& issue) noexcept {
    try {
        (void)notifier_->send(VideoCaptureWorkerFailed{issue});
    } catch (...) {
    }
}

void DefaultVideoCaptureWorker::shutdown_worker() noexcept {
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

void DefaultVideoCaptureWorker::cancel_pending_commands() noexcept {
    std::deque<ControlCommand> commands;
    {
        std::lock_guard lock{mutex_};
        commands.swap(commands_);
    }
    auto cancelled = StartResult{std::unexpected{worker_issue(
        VideoCaptureWorkerOperation::ThreadInitialization,
        "video capture worker is shutting down")}};
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

void DefaultVideoCaptureWorker::mark_module_alive() noexcept {
    {
        std::lock_guard lock{mutex_};
        module_state_ = ModuleState::Alive;
    }
    cv_.notify_all();
}

void DefaultVideoCaptureWorker::mark_module_stopped(
    std::string bootstrap_error) noexcept {
    {
        std::lock_guard lock{mutex_};
        bootstrap_error_ = std::move(bootstrap_error);
        module_state_ = ModuleState::Stopped;
    }
    cv_.notify_all();
}

void DefaultVideoCaptureWorker::reset_session_stats(
    const contracts::capture::DesktopCaptureInfo& source) noexcept {
    std::lock_guard lock{mutex_};
    stats_.source_width = source.width;
    stats_.source_height = source.height;
}

void DefaultVideoCaptureWorker::record_processed_tick() noexcept {
    std::lock_guard lock{mutex_};
    ++stats_.processed_ticks;
}

void DefaultVideoCaptureWorker::record_capture_time(
    const Clock::duration elapsed) noexcept {
    const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
    std::lock_guard lock{mutex_};
    ++stats_.capture_calls;
    stats_.total_capture_time += duration;
    stats_.maximum_capture_time = std::max(stats_.maximum_capture_time, duration);
}

void DefaultVideoCaptureWorker::record_image(const BgraFrameBuffer& image,
                                             const bool recovered) noexcept {
    std::lock_guard lock{mutex_};
    ++stats_.new_desktop_images;
    stats_.new_image_bytes += image.bgra.size();
    if (stats_.source_width != image.width || stats_.source_height != image.height) {
        ++stats_.source_size_changes;
    }
    stats_.source_width = image.width;
    stats_.source_height = image.height;
    stats_.successful_recoveries += recovered ? 1U : 0U;
}

void DefaultVideoCaptureWorker::record_no_change(const bool recovered) noexcept {
    std::lock_guard lock{mutex_};
    ++stats_.no_change_observations;
    stats_.successful_recoveries += recovered ? 1U : 0U;
}

void DefaultVideoCaptureWorker::record_temporary(const bool new_episode) noexcept {
    std::lock_guard lock{mutex_};
    ++stats_.temporarily_unavailable_observations;
    stats_.recovery_episodes += new_episode ? 1U : 0U;
}

void DefaultVideoCaptureWorker::record_published(
    const FrameContent content,
    const CapturedVideoFramePushResult result) noexcept {
    std::lock_guard lock{mutex_};
    ++stats_.published_frames;
    stats_.repeated_frames += content == FrameContent::Repeated ? 1U : 0U;
    stats_.store_replaced_frames +=
        result == CapturedVideoFramePushResult::ReplacedOldest ? 1U : 0U;
}

void DefaultVideoCaptureWorker::record_missing_frame() noexcept {
    std::lock_guard lock{mutex_};
    ++stats_.missing_initial_frames;
}

void DefaultVideoCaptureWorker::record_schedule(
    const std::uint64_t skipped_ticks) noexcept {
    std::lock_guard lock{mutex_};
    stats_.scheduler_skipped_ticks += skipped_ticks;
}

void DefaultVideoCaptureWorker::record_fatal_failure() noexcept {
    std::lock_guard lock{mutex_};
    ++stats_.fatal_failures;
}

}  // namespace semilive::publisher::domain
