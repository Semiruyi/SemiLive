#include <semilive/receiver/domain/worker/default_video_receive_worker.hpp>

#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace semilive::receiver::domain {
namespace {

VideoReceiveWorkerIssue make_issue(
    const VideoReceiveWorkerOperation operation,
    std::string message) {
    return {operation, std::move(message)};
}

}  // namespace

struct DefaultVideoReceiveWorker::Impl {
    ~Impl() {
        stop_noexcept();
    }

    [[nodiscard]] VideoReceiveStartResult start();
    [[nodiscard]] VideoReceiveStopResult stop();
    [[nodiscard]] VideoReceiveWorkerState state() const noexcept;
    [[nodiscard]] VideoReceiveWorkerStats stats() const noexcept;
    [[nodiscard]] VideoReceiveWaitResult wait_for_terminal_for(
        std::chrono::milliseconds timeout);

    void run(std::stop_token stop_token) noexcept;
    void stop_noexcept() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable_any state_changed_;
    std::jthread thread_;
    VideoReceiveWorkerStats stats_;
    std::optional<VideoReceiveWorkerIssue> last_issue_;
};

VideoReceiveStartResult DefaultVideoReceiveWorker::Impl::start() {
    std::unique_lock lock{mutex_};
    if (stats_.state != VideoReceiveWorkerState::Idle) {
        return std::unexpected{make_issue(
            VideoReceiveWorkerOperation::Control,
            "video receive worker must be idle before start")};
    }

    stats_.state = VideoReceiveWorkerState::Starting;
    ++stats_.session_id;
    last_issue_.reset();

    try {
        thread_ = std::jthread{
            [this](const std::stop_token stop_token) { run(stop_token); }};
    } catch (const std::exception& error) {
        stats_.state = VideoReceiveWorkerState::Idle;
        ++stats_.start_failures;
        last_issue_ = make_issue(VideoReceiveWorkerOperation::StartSession,
                                 error.what());
        return std::unexpected{*last_issue_};
    } catch (...) {
        stats_.state = VideoReceiveWorkerState::Idle;
        ++stats_.start_failures;
        last_issue_ = make_issue(
            VideoReceiveWorkerOperation::StartSession,
            "unknown exception while creating receive session thread");
        return std::unexpected{*last_issue_};
    }

    state_changed_.wait(lock, [this] {
        return stats_.state != VideoReceiveWorkerState::Starting;
    });
    if (stats_.state == VideoReceiveWorkerState::Failed) {
        return std::unexpected{*last_issue_};
    }
    return VideoReceiveStarted{stats_.session_id};
}

VideoReceiveStopResult DefaultVideoReceiveWorker::Impl::stop() {
    std::unique_lock lock{mutex_};
    if (stats_.state == VideoReceiveWorkerState::Idle) {
        return VideoReceiveStopped{stats_.session_id};
    }

    const auto stopped_session = stats_.session_id;
    stats_.state = VideoReceiveWorkerState::Stopping;
    thread_.request_stop();
    lock.unlock();

    if (thread_.joinable()) {
        thread_.join();
    }

    lock.lock();
    stats_.state = VideoReceiveWorkerState::Idle;
    ++stats_.completed_sessions;
    state_changed_.notify_all();
    return VideoReceiveStopped{stopped_session};
}

VideoReceiveWorkerState DefaultVideoReceiveWorker::Impl::state() const noexcept {
    std::lock_guard lock{mutex_};
    return stats_.state;
}

VideoReceiveWorkerStats DefaultVideoReceiveWorker::Impl::stats() const noexcept {
    std::lock_guard lock{mutex_};
    return stats_;
}

VideoReceiveWaitResult
DefaultVideoReceiveWorker::Impl::wait_for_terminal_for(
    const std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    const auto terminal = state_changed_.wait_for(lock, timeout, [this] {
        return stats_.state == VideoReceiveWorkerState::Idle ||
               stats_.state == VideoReceiveWorkerState::Failed;
    });
    if (!terminal) {
        return {VideoReceiveWaitStatus::Timeout, std::nullopt};
    }
    if (stats_.state == VideoReceiveWorkerState::Failed) {
        return {VideoReceiveWaitStatus::Failed, last_issue_};
    }
    return {VideoReceiveWaitStatus::Idle, std::nullopt};
}

void DefaultVideoReceiveWorker::Impl::run(
    const std::stop_token stop_token) noexcept {
    std::unique_lock lock{mutex_};
    stats_.state = VideoReceiveWorkerState::Running;
    ++stats_.started_sessions;
    state_changed_.notify_all();

    state_changed_.wait(lock, stop_token, [] { return false; });
}

void DefaultVideoReceiveWorker::Impl::stop_noexcept() noexcept {
    try {
        (void)stop();
    } catch (...) {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
    }
}

DefaultVideoReceiveWorker::DefaultVideoReceiveWorker()
    : impl_{std::make_unique<Impl>()} {}

DefaultVideoReceiveWorker::~DefaultVideoReceiveWorker() = default;

VideoReceiveStartResult DefaultVideoReceiveWorker::start() {
    return impl_->start();
}

VideoReceiveStopResult DefaultVideoReceiveWorker::stop() {
    return impl_->stop();
}

VideoReceiveWorkerState DefaultVideoReceiveWorker::state() const noexcept {
    return impl_->state();
}

VideoReceiveWorkerStats DefaultVideoReceiveWorker::stats() const noexcept {
    return impl_->stats();
}

VideoReceiveWaitResult DefaultVideoReceiveWorker::wait_for_terminal_for(
    const std::chrono::milliseconds timeout) {
    return impl_->wait_for_terminal_for(timeout);
}

}  // namespace semilive::receiver::domain
