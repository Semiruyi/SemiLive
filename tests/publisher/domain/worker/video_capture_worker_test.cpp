#include "publisher/domain/resource/captured_video_frame_store/captured_video_frame_store.hpp"
#include "publisher/domain/worker/video_capture_worker/default_video_capture_worker.hpp"
#include "publisher/domain/worker/video_capture_worker/video_capture_worker_events.hpp"
#include "publisher/infrastructure/capture/synthetic_desktop_capture_backend.hpp"
#include "publisher/infrastructure/notifier/default_notifier.hpp"

#ifdef _WIN32
#include "publisher/infrastructure/capture/dxgi_desktop_capture_backend.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace capture_contract = semilive::publisher::contracts::capture;
namespace capture_infra = semilive::publisher::infra::capture;
namespace publisher = semilive::publisher::domain;

using semilive::publisher::infra::DefaultNotifier;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename Predicate>
bool wait_until(Predicate predicate, const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

capture_contract::DesktopImage image(const std::byte value = std::byte{0x31}) {
    return capture_contract::DesktopImage{
        std::vector<std::byte>(8, value), 2, 1, 8};
}

publisher::VideoCaptureSessionConfig config(
    const std::chrono::milliseconds recovery_timeout = 100ms) {
    return publisher::VideoCaptureSessionConfig{
        {},
        publisher::SessionTimeline{std::chrono::steady_clock::now()},
        publisher::FrameRate{200, 1},
        recovery_timeout,
    };
}

struct RecordingBackendState {
    std::mutex mutex;
    std::thread::id controller_thread;
    std::thread::id worker_thread;
    const std::byte* emitted_pixels = nullptr;
    std::uint64_t open_calls = 0;
    std::uint64_t capture_calls = 0;
    std::uint64_t close_calls = 0;
    bool thread_consistent = true;
};

class RecordingBackend final : public capture_contract::DesktopCaptureBackend {
public:
    explicit RecordingBackend(std::shared_ptr<RecordingBackendState> state)
        : state_(std::move(state)) {}

    std::expected<capture_contract::DesktopCaptureInfo,
                  capture_contract::DesktopCaptureIssue>
    open(const capture_contract::DesktopCaptureConfig&) override {
        std::lock_guard lock{state_->mutex};
        state_->worker_thread = std::this_thread::get_id();
        state_->thread_consistent =
            state_->worker_thread != state_->controller_thread;
        ++state_->open_calls;
        emitted_ = false;
        open_ = true;
        return capture_contract::DesktopCaptureInfo{"Recording Output", 2, 1};
    }

    capture_contract::DesktopCaptureResult capture_latest() override {
        record_call(false);
        if (!open_) {
            return std::unexpected{capture_contract::DesktopCaptureIssue{
                capture_contract::DesktopCaptureOperation::Acquire, 0, "not open"}};
        }
        if (emitted_) {
            return capture_contract::DesktopCaptureObservation{
                capture_contract::DesktopNoChange{}};
        }
        emitted_ = true;
        auto desktop = image();
        {
            std::lock_guard lock{state_->mutex};
            state_->emitted_pixels = desktop.bgra.data();
        }
        return capture_contract::DesktopCaptureObservation{std::move(desktop)};
    }

    void close() noexcept override {
        if (!open_) {
            return;
        }
        record_call(true);
        open_ = false;
    }

private:
    void record_call(const bool closing) noexcept {
        std::lock_guard lock{state_->mutex};
        state_->thread_consistent = state_->thread_consistent &&
                                    state_->worker_thread == std::this_thread::get_id();
        if (closing) {
            ++state_->close_calls;
        } else {
            ++state_->capture_calls;
        }
    }

    std::shared_ptr<RecordingBackendState> state_;
    bool emitted_ = false;
    bool open_ = false;
};

std::unique_ptr<capture_infra::SyntheticDesktopCaptureBackend>
synthetic_backend(capture_infra::SyntheticDesktopCaptureScript script) {
    return std::make_unique<capture_infra::SyntheticDesktopCaptureBackend>(
        std::move(script));
}

void worker_uses_one_thread_and_shares_repeated_pixels() {
    auto notifier = std::make_shared<DefaultNotifier>();
    publisher::CapturedVideoFrameStore store{notifier, 64};
    auto backend_state = std::make_shared<RecordingBackendState>();
    backend_state->controller_thread = std::this_thread::get_id();
    publisher::DefaultVideoCaptureWorker worker{
        std::make_unique<RecordingBackend>(backend_state), store, notifier};

    require(worker.state() == publisher::VideoCaptureWorkerState::Idle,
            "constructed worker must be idle");
    const auto started = worker.start(config());
    require(started && started->source.output_name == "Recording Output",
            "worker must report the opened output");
    require(wait_until([&store] { return store.size() >= 2; }),
            "worker must publish a new frame and a repeated frame");
    worker.stop();

    auto first = store.try_pop();
    auto second = store.try_pop();
    require(first && second && first->image == second->image,
            "repeated frames must share immutable pixels");
    require(first->sequence == 0 && second->sequence == 1,
            "published sequence must be contiguous");
    require(second->presentation_time > first->presentation_time,
            "repeated frame PTS must advance");
    std::lock_guard lock{backend_state->mutex};
    require(first->image->bgra.data() == backend_state->emitted_pixels,
            "worker must move backend pixels without a full-frame copy");
    require(backend_state->thread_consistent && backend_state->close_calls == 1,
            "backend lifecycle must stay on one worker thread");
}

void worker_rejects_invalid_start_state_and_supports_restart() {
    auto notifier = std::make_shared<DefaultNotifier>();
    publisher::CapturedVideoFrameStore store{notifier, 64};
    auto state = std::make_shared<RecordingBackendState>();
    state->controller_thread = std::this_thread::get_id();
    publisher::DefaultVideoCaptureWorker worker{
        std::make_unique<RecordingBackend>(state), store, notifier};

    require(worker.start(config()).has_value(), "first session must start");
    const auto duplicate = worker.start(config());
    require(!duplicate && duplicate.error().operation ==
                              publisher::VideoCaptureWorkerOperation::Control,
            "starting a running worker must return a control error");
    worker.stop();
    (void)store.clear();

    require(worker.start(config()).has_value(), "stopped worker must restart");
    require(wait_until([&store] { return !store.empty(); }),
            "restarted worker must publish a frame");
    worker.stop();
    const auto restarted = store.try_pop();
    require(restarted && restarted->sequence == 0,
            "a new session must restart frame sequence at zero");
    worker.stop();
}

void start_failure_returns_error_and_restores_idle() {
    auto notifier = std::make_shared<DefaultNotifier>();
    publisher::CapturedVideoFrameStore store{notifier};
    capture_infra::SyntheticDesktopCaptureScript script;
    publisher::DefaultVideoCaptureWorker worker{
        synthetic_backend(std::move(script)), store, notifier};

    auto invalid = config();
    invalid.capture.output.selection = capture_contract::DesktopOutputSelection::Index;
    invalid.capture.output.index = 1;
    const auto result = worker.start(std::move(invalid));
    require(!result && result.error().operation ==
                           publisher::VideoCaptureWorkerOperation::OpenBackend,
            "backend open failure must be returned by start");
    require(result.error().capture_issue &&
                result.error().capture_issue->operation ==
                    capture_contract::DesktopCaptureOperation::Open,
            "start failure must preserve the backend issue");
    require(worker.state() == publisher::VideoCaptureWorkerState::Idle,
            "start failure must restore idle state");
}

capture_infra::SyntheticDesktopCaptureScript fatal_script() {
    capture_infra::SyntheticDesktopCaptureScript script;
    script.initial_width = 2;
    script.initial_height = 1;
    script.steps.emplace_back(capture_infra::SyntheticDesktopCaptureFailure{
        capture_contract::DesktopCaptureIssue{
            capture_contract::DesktopCaptureOperation::Acquire,
            77,
            "fatal scripted capture",
        }});
    return script;
}

void runtime_failure_notifies_once_and_requires_stop() {
    auto notifier = std::make_shared<DefaultNotifier>();
    publisher::CapturedVideoFrameStore store{notifier};
    std::atomic_uint32_t failures{0};
    auto subscription = notifier->subscribe<publisher::VideoCaptureWorkerFailed>(
        [&failures](const publisher::VideoCaptureWorkerFailed& event) {
            if (event.issue.operation == publisher::VideoCaptureWorkerOperation::Capture &&
                event.issue.capture_issue && event.issue.capture_issue->native_code == 77) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    publisher::DefaultVideoCaptureWorker worker{
        synthetic_backend(fatal_script()), store, notifier};

    require(worker.start(config()).has_value(), "fatal session must start first");
    require(wait_until([&failures] {
                return failures.load(std::memory_order_relaxed) == 1;
            }),
            "runtime failure must notify the controller");
    require(worker.state() == publisher::VideoCaptureWorkerState::Failed,
            "runtime fatal error must enter failed state");
    require(worker.stats().fatal_failures == 1,
            "runtime fatal error must be counted once");
    worker.stop();
    require(worker.state() == publisher::VideoCaptureWorkerState::Idle,
            "stop must clean a failed session back to idle");
    require(subscription->active(), "failure subscription must remain active");
}

capture_infra::SyntheticDesktopCaptureScript recovery_script() {
    capture_infra::SyntheticDesktopCaptureScript script;
    script.initial_width = 2;
    script.initial_height = 1;
    script.steps.emplace_back(image());
    const auto issue = capture_contract::DesktopCaptureIssue{
        capture_contract::DesktopCaptureOperation::Reinitialize, 91, "temporary"};
    script.steps.emplace_back(capture_contract::DesktopTemporarilyUnavailable{issue});
    script.steps.emplace_back(capture_contract::DesktopTemporarilyUnavailable{issue});
    script.steps.emplace_back(capture_contract::DesktopNoChange{});
    return script;
}

void temporary_failure_reuses_pixels_and_records_recovery() {
    auto notifier = std::make_shared<DefaultNotifier>();
    publisher::CapturedVideoFrameStore store{notifier, 64};
    publisher::DefaultVideoCaptureWorker worker{
        synthetic_backend(recovery_script()), store, notifier};

    require(worker.start(config()).has_value(), "recovery session must start");
    require(wait_until([&worker] {
                return worker.stats().successful_recoveries == 1;
            }),
            "a normal observation must end the recovery episode");
    worker.stop();

    const auto stats = worker.stats();
    require(stats.recovery_episodes == 1 &&
                stats.temporarily_unavailable_observations == 2,
            "temporary observations must form one recovery episode");
    require(stats.repeated_frames >= 2,
            "temporary unavailability must reuse the cached image");
}

capture_infra::SyntheticDesktopCaptureScript unavailable_script() {
    capture_infra::SyntheticDesktopCaptureScript script;
    script.initial_width = 2;
    script.initial_height = 1;
    const auto issue = capture_contract::DesktopCaptureIssue{
        capture_contract::DesktopCaptureOperation::Reinitialize, 92, "unavailable"};
    for (int index = 0; index < 20; ++index) {
        script.steps.emplace_back(
            capture_contract::DesktopTemporarilyUnavailable{issue});
    }
    return script;
}

void recovery_timeout_becomes_a_runtime_failure() {
    auto notifier = std::make_shared<DefaultNotifier>();
    publisher::CapturedVideoFrameStore store{notifier};
    std::atomic_bool recovery_failed{false};
    auto subscription = notifier->subscribe<publisher::VideoCaptureWorkerFailed>(
        [&recovery_failed](const publisher::VideoCaptureWorkerFailed& event) {
            if (event.issue.operation == publisher::VideoCaptureWorkerOperation::Recovery) {
                recovery_failed.store(true, std::memory_order_relaxed);
            }
        });
    publisher::DefaultVideoCaptureWorker worker{
        synthetic_backend(unavailable_script()), store, notifier};

    require(worker.start(config(12ms)).has_value(),
            "unavailable session must start before recovery times out");
    require(wait_until([&recovery_failed] {
                return recovery_failed.load(std::memory_order_relaxed);
            }),
            "continuous unavailability must become a recovery failure");
    require(worker.state() == publisher::VideoCaptureWorkerState::Failed,
            "recovery timeout must enter failed state");
    require(worker.stats().recovery_episodes == 1,
            "continuous unavailability must remain one recovery episode");
    worker.stop();
    require(subscription->active(), "recovery failure subscription must remain active");
}

void stop_wakes_a_far_frame_deadline() {
    auto notifier = std::make_shared<DefaultNotifier>();
    publisher::CapturedVideoFrameStore store{notifier};
    auto state = std::make_shared<RecordingBackendState>();
    state->controller_thread = std::this_thread::get_id();
    publisher::DefaultVideoCaptureWorker worker{
        std::make_unique<RecordingBackend>(state), store, notifier};
    auto slow_config = config();
    slow_config.frame_rate = publisher::FrameRate{1, 10};

    require(worker.start(std::move(slow_config)).has_value(),
            "slow frame-rate session must start");
    require(wait_until([&worker] { return worker.stats().capture_calls >= 1; }),
            "slow session must process its initial tick");
    const auto stop_started = std::chrono::steady_clock::now();
    worker.stop();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;
    require(stop_elapsed < 500ms,
            "stop must wake the worker instead of waiting for the next deadline");
}

void destructor_stops_a_running_session() {
    auto notifier = std::make_shared<DefaultNotifier>();
    publisher::CapturedVideoFrameStore store{notifier};
    auto state = std::make_shared<RecordingBackendState>();
    state->controller_thread = std::this_thread::get_id();
    {
        publisher::DefaultVideoCaptureWorker worker{
            std::make_unique<RecordingBackend>(state), store, notifier};
        require(worker.start(config()).has_value(), "destructor test session must start");
    }

    std::lock_guard lock{state->mutex};
    require(state->close_calls == 1 && state->thread_consistent,
            "destructor must close the backend on the worker thread");
}

#ifdef _WIN32
void dxgi_worker_integration() {
    auto notifier = std::make_shared<DefaultNotifier>();
    publisher::CapturedVideoFrameStore store{notifier, 64};
    publisher::DefaultVideoCaptureWorker worker{
        std::make_unique<capture_infra::DxgiDesktopCaptureBackend>(), store, notifier};
    auto live_config = config(5s);
    live_config.frame_rate = publisher::FrameRate{30, 1};

    const auto started = worker.start(std::move(live_config));
    require(started.has_value(), "DXGI worker must open the primary desktop");
    require(wait_until([&store] { return !store.empty(); }, 5s),
            "DXGI worker must publish a desktop frame");
    worker.stop();

    const auto frame = store.try_pop();
    require(frame && frame->image && !frame->image->bgra.empty(),
            "DXGI worker frame must own BGRA pixels");
    require(frame->image->width == started->source.width &&
                frame->image->height == started->source.height,
            "DXGI worker frame must match the opened output dimensions");
}
#endif

}  // namespace

int main(const int argc, const char* const argv[]) {
    try {
#ifdef _WIN32
        if (argc == 2 && std::string_view{argv[1]} == "--integration") {
            dxgi_worker_integration();
            std::cout << "publisher DXGI video capture worker integration passed\n";
            return EXIT_SUCCESS;
        }
#else
        (void)argc;
        (void)argv;
#endif
        worker_uses_one_thread_and_shares_repeated_pixels();
        worker_rejects_invalid_start_state_and_supports_restart();
        start_failure_returns_error_and_restores_idle();
        runtime_failure_notifies_once_and_requires_stop();
        temporary_failure_reuses_pixels_and_records_recovery();
        recovery_timeout_becomes_a_runtime_failure();
        stop_wakes_a_far_frame_deadline();
        destructor_stops_a_running_session();
    } catch (const std::exception& error) {
        std::cerr << "publisher video capture worker test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher video capture worker tests passed\n";
    return EXIT_SUCCESS;
}
