#include <semilive/publisher/application/publisher_controller/default_publisher_controller.hpp>
#include <semilive/publisher/domain/worker/video_capture_worker/video_capture_worker_events.hpp>
#include <semilive/publisher/domain/worker/video_encoder_worker/video_encoder_worker_events.hpp>
#include <semilive/publisher/domain/worker/video_output_worker/video_output_worker_events.hpp>
#include "publisher/support/notifier/synchronous_notifier.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace app = semilive::publisher::application;
namespace domain = semilive::publisher::domain;
namespace model = semilive::publisher::model;

using semilive::publisher::test_support::SynchronousNotifier;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

struct RecordedCall {
    std::string name;
    std::thread::id thread_id;
};

class CallLog {
public:
    void record(std::string name) {
        {
            std::lock_guard lock{mutex_};
            calls_.push_back({std::move(name), std::this_thread::get_id()});
        }
        cv_.notify_all();
    }

    [[nodiscard]] std::vector<RecordedCall> snapshot() const {
        std::lock_guard lock{mutex_};
        return calls_;
    }

    [[nodiscard]] bool wait_for(const std::string_view name,
                                const std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock{mutex_};
        return cv_.wait_for(lock, timeout, [this, name] {
            return std::ranges::any_of(
                calls_, [name](const RecordedCall& call) {
                    return call.name == name;
                });
        });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<RecordedCall> calls_;
};

class RecordingFrameStoreControl final
    : public domain::CapturedVideoFrameStoreControl {
public:
    RecordingFrameStoreControl(CallLog& calls, const std::size_t size)
        : calls_{&calls}, size_{size}, peak_size_{size} {}

    [[nodiscard]] std::size_t clear() noexcept override {
        calls_->record("frame.clear");
        const auto previous = size_;
        size_ = 0;
        return previous;
    }

    [[nodiscard]] std::size_t size() const noexcept override {
        return size_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept override {
        return 2;
    }

    [[nodiscard]] std::size_t peak_size() const noexcept override {
        return peak_size_;
    }

    [[nodiscard]] std::uint64_t replaced_count() const noexcept override {
        return 0;
    }

private:
    CallLog* calls_ = nullptr;
    std::size_t size_ = 0;
    std::size_t peak_size_ = 0;
};

class RecordingAccessUnitQueueControl final
    : public domain::EncodedVideoAccessUnitQueueControl {
public:
    RecordingAccessUnitQueueControl(CallLog& calls, const std::size_t size)
        : calls_{&calls}, size_{size}, peak_size_{size} {}

    [[nodiscard]] std::size_t clear() noexcept override {
        calls_->record("queue.clear");
        const auto previous = size_;
        size_ = 0;
        return previous;
    }

    [[nodiscard]] std::size_t size() const noexcept override {
        return size_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept override {
        return 4;
    }

    [[nodiscard]] std::size_t peak_size() const noexcept override {
        return peak_size_;
    }

private:
    CallLog* calls_ = nullptr;
    std::size_t size_ = 0;
    std::size_t peak_size_ = 0;
};

class RecordingCaptureWorker final : public domain::VideoCaptureWorker {
public:
    RecordingCaptureWorker(CallLog& calls,
                           std::shared_ptr<SynchronousNotifier> notifier)
        : calls_{&calls}, notifier_{std::move(notifier)} {}

    [[nodiscard]] std::expected<domain::VideoCaptureStarted,
                                domain::VideoCaptureWorkerIssue>
    start(domain::VideoCaptureSessionConfig config) override {
        calls_->record("capture.start");
        last_config_ = config;
        if (start_issue_) {
            return std::unexpected{*start_issue_};
        }
        state_ = domain::VideoCaptureWorkerState::Running;
        const auto track_start = std::chrono::steady_clock::now();
        return domain::VideoCaptureStarted{
            {"Scripted Desktop", 1280, 720},
            track_start,
            config.timeline.media_time_at(track_start),
        };
    }

    void stop() override {
        calls_->record("capture.stop");
        state_ = domain::VideoCaptureWorkerState::Idle;
    }

    [[nodiscard]] domain::VideoCaptureWorkerState state() const noexcept override {
        return state_;
    }

    [[nodiscard]] domain::VideoCaptureWorkerStats stats() const noexcept override {
        return stats_;
    }

    void set_start_issue(domain::VideoCaptureWorkerIssue issue) {
        start_issue_ = std::move(issue);
    }

    void fail(domain::VideoCaptureWorkerIssue issue) {
        state_ = domain::VideoCaptureWorkerState::Failed;
        ++stats_.fatal_failures;
        (void)notifier_->send(domain::VideoCaptureWorkerFailed{std::move(issue)});
    }

    [[nodiscard]] const std::optional<domain::VideoCaptureSessionConfig>&
    last_config() const noexcept {
        return last_config_;
    }

private:
    CallLog* calls_ = nullptr;
    std::shared_ptr<SynchronousNotifier> notifier_;
    domain::VideoCaptureWorkerState state_ =
        domain::VideoCaptureWorkerState::Idle;
    domain::VideoCaptureWorkerStats stats_;
    std::optional<domain::VideoCaptureWorkerIssue> start_issue_;
    std::optional<domain::VideoCaptureSessionConfig> last_config_;
};

class RecordingEncoderWorker final : public domain::VideoEncoderWorker {
public:
    RecordingEncoderWorker(CallLog& calls,
                           std::shared_ptr<SynchronousNotifier> notifier)
        : calls_{&calls}, notifier_{std::move(notifier)} {}

    [[nodiscard]] std::expected<domain::VideoEncoderStarted,
                                domain::VideoEncoderWorkerIssue>
    start(domain::VideoEncoderSessionConfig config) override {
        calls_->record("encoder.start");
        last_config_ = config;
        if (start_issue_) {
            return std::unexpected{*start_issue_};
        }
        std::lock_guard lock{mutex_};
        aborted_ = false;
        state_ = domain::VideoEncoderWorkerState::Running;
        return domain::VideoEncoderStarted{{
            config.encoder.output,
            config.encoder.frame_rate,
            config.encoder.target_bit_rate,
            config.encoder.gop_size,
            1,
            "scripted encoder",
        }};
    }

    void stop(const domain::VideoEncoderStopMode mode) override {
        if (mode == domain::VideoEncoderStopMode::Abort) {
            calls_->record("encoder.abort");
            {
                std::lock_guard lock{mutex_};
                aborted_ = true;
                state_ = domain::VideoEncoderWorkerState::Idle;
            }
            cv_.notify_all();
            return;
        }

        calls_->record("encoder.drain");
        std::unique_lock lock{mutex_};
        if (state_ == domain::VideoEncoderWorkerState::Failed) {
            return;
        }
        state_ = domain::VideoEncoderWorkerState::Draining;
        drain_entered_ = true;
        cv_.notify_all();
        if (block_drain_) {
            cv_.wait(lock, [this] { return drain_released_ || aborted_; });
        }
        if (!aborted_) {
            state_ = domain::VideoEncoderWorkerState::Idle;
        }
    }

    [[nodiscard]] domain::VideoEncoderWorkerState state() const noexcept override {
        std::lock_guard lock{mutex_};
        return state_;
    }

    [[nodiscard]] domain::VideoEncoderWorkerStats stats() const noexcept override {
        std::lock_guard lock{mutex_};
        return stats_;
    }

    void set_start_issue(domain::VideoEncoderWorkerIssue issue) {
        start_issue_ = std::move(issue);
    }

    void block_drain() {
        std::lock_guard lock{mutex_};
        block_drain_ = true;
    }

    [[nodiscard]] bool wait_for_drain(
        const std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock{mutex_};
        return cv_.wait_for(lock, timeout,
                            [this] { return drain_entered_; });
    }

    void fail(domain::VideoEncoderWorkerIssue issue) {
        {
            std::lock_guard lock{mutex_};
            state_ = domain::VideoEncoderWorkerState::Failed;
            stats_.last_issue = issue;
            ++stats_.fatal_failures;
        }
        (void)notifier_->send(domain::VideoEncoderWorkerFailed{std::move(issue)});
    }

    [[nodiscard]] const std::optional<domain::VideoEncoderSessionConfig>&
    last_config() const noexcept {
        return last_config_;
    }

private:
    CallLog* calls_ = nullptr;
    std::shared_ptr<SynchronousNotifier> notifier_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    domain::VideoEncoderWorkerState state_ =
        domain::VideoEncoderWorkerState::Idle;
    domain::VideoEncoderWorkerStats stats_;
    std::optional<domain::VideoEncoderWorkerIssue> start_issue_;
    std::optional<domain::VideoEncoderSessionConfig> last_config_;
    bool block_drain_ = false;
    bool drain_entered_ = false;
    bool drain_released_ = false;
    bool aborted_ = false;
};

class RecordingOutputWorker final : public domain::VideoOutputWorker {
public:
    RecordingOutputWorker(CallLog& calls,
                          std::shared_ptr<SynchronousNotifier> notifier)
        : calls_{&calls}, notifier_{std::move(notifier)} {}

    [[nodiscard]] std::expected<domain::VideoOutputStarted,
                                domain::VideoOutputWorkerIssue>
    start() override {
        calls_->record("output.start");
        if (start_issue_) {
            return std::unexpected{*start_issue_};
        }
        std::lock_guard lock{mutex_};
        state_ = domain::VideoOutputWorkerState::Running;
        return domain::VideoOutputStarted{{"memory://publisher"}};
    }

    void stop(const domain::VideoOutputStopMode mode) override {
        calls_->record(mode == domain::VideoOutputStopMode::Drain
                           ? "output.drain"
                           : "output.abort");
        std::lock_guard lock{mutex_};
        if (mode == domain::VideoOutputStopMode::Abort ||
            state_ != domain::VideoOutputWorkerState::Failed) {
            state_ = domain::VideoOutputWorkerState::Idle;
        }
    }

    [[nodiscard]] domain::VideoOutputWorkerState state() const noexcept override {
        std::lock_guard lock{mutex_};
        return state_;
    }

    [[nodiscard]] domain::VideoOutputWorkerStats stats() const noexcept override {
        std::lock_guard lock{mutex_};
        return stats_;
    }

    void set_start_issue(domain::VideoOutputWorkerIssue issue) {
        start_issue_ = std::move(issue);
    }

    void fail(domain::VideoOutputWorkerIssue issue) {
        {
            std::lock_guard lock{mutex_};
            state_ = domain::VideoOutputWorkerState::Failed;
            stats_.last_issue = issue;
            ++stats_.fatal_failures;
        }
        (void)notifier_->send(domain::VideoOutputWorkerFailed{std::move(issue)});
    }

private:
    CallLog* calls_ = nullptr;
    std::shared_ptr<SynchronousNotifier> notifier_;
    mutable std::mutex mutex_;
    domain::VideoOutputWorkerState state_ =
        domain::VideoOutputWorkerState::Idle;
    domain::VideoOutputWorkerStats stats_;
    std::optional<domain::VideoOutputWorkerIssue> start_issue_;
};

struct Fixture {
    Fixture(const std::size_t frame_count = 0,
            const std::size_t access_unit_count = 0)
        : notifier{std::make_shared<SynchronousNotifier>()},
          frame_store{calls, frame_count},
          access_unit_queue{calls, access_unit_count},
          capture{calls, notifier},
          encoder{calls, notifier},
          output{calls, notifier} {}

    [[nodiscard]] app::PublisherVideoSessionPlan plan() const {
        app::PublisherVideoSessionPlan result;
        result.capture.compose_pointer = false;
        result.recovery_timeout = 3s;
        result.encoder.output = {1280, 720};
        result.encoder.frame_rate = {60, 1};
        result.encoder.target_bit_rate = 2'000'000;
        result.encoder.gop_size = 30;
        return result;
    }

    [[nodiscard]] app::PublisherVideoPipeline pipeline() {
        return {capture, encoder, output, frame_store, access_unit_queue};
    }

    CallLog calls;
    std::shared_ptr<SynchronousNotifier> notifier;
    RecordingFrameStoreControl frame_store;
    RecordingAccessUnitQueueControl access_unit_queue;
    RecordingCaptureWorker capture;
    RecordingEncoderWorker encoder;
    RecordingOutputWorker output;
};

std::vector<std::string> call_names(const CallLog& calls) {
    std::vector<std::string> result;
    for (const auto& call : calls.snapshot()) {
        result.push_back(call.name);
    }
    return result;
}

std::size_t call_index(const std::vector<std::string>& calls,
                       const std::string_view name) {
    const auto found = std::ranges::find(calls, name);
    require(found != calls.end(), "expected controller call was not recorded");
    return static_cast<std::size_t>(std::distance(calls.begin(), found));
}

domain::VideoEncoderWorkerIssue encoder_failure() {
    return {domain::VideoEncoderWorkerOperation::Encode,
            std::nullopt,
            "scripted encoder failure"};
}

domain::VideoOutputWorkerIssue output_failure() {
    return {domain::VideoOutputWorkerOperation::Output,
            std::nullopt,
            "scripted output failure"};
}

void normal_lifecycle_is_ordered_and_repeatable() {
    Fixture fixture{2, 3};
    app::DefaultPublisherController controller{
        fixture.plan(), fixture.pipeline(), fixture.notifier};

    const auto started = controller.start_publishing();
    require(started.has_value(), "publisher session must start");
    require(started->session_id == 1,
            "first publisher session must receive id one");
    require(started->output.output.output_name == "memory://publisher",
            "controller must preserve output startup information");
    require(fixture.capture.last_config() &&
                fixture.capture.last_config()->timeline.origin() ==
                    started->timeline.origin() &&
                fixture.capture.last_config()->frame_rate ==
                    fixture.plan().encoder.frame_rate,
            "controller must pass one timeline and frame rate to capture");

    const auto duplicate_start = controller.start_publishing();
    require(!duplicate_start &&
                duplicate_start.error().operation ==
                    app::PublisherControllerOperation::Control &&
                controller.state() == app::PublisherControllerState::Running,
            "duplicate start must be rejected without disturbing the session");
    require(controller.wait_for_terminal_for(10ms).status ==
                app::PublisherWaitStatus::Timeout,
            "a healthy running session must not appear terminal");

    const auto stopped = controller.stop_publishing();
    require(stopped.has_value(), "publisher session must drain normally");
    require(controller.state() == app::PublisherControllerState::Idle,
            "normal stop must return the controller to idle");

    const auto calls = call_names(fixture.calls);
    require(call_index(calls, "output.start") <
                call_index(calls, "encoder.start") &&
                call_index(calls, "encoder.start") <
                    call_index(calls, "capture.start"),
            "workers must start from consumer to producer");
    require(call_index(calls, "capture.stop") <
                call_index(calls, "encoder.drain") &&
                call_index(calls, "encoder.drain") <
                    call_index(calls, "output.drain"),
            "workers must stop from producer to consumer");

    const auto stats = controller.stats();
    require(stats.started_sessions == 1 && stats.completed_sessions == 1 &&
                stats.failed_sessions == 0,
            "normal lifecycle must update controller session statistics");
    require(stats.cleared_frames == 2 && stats.cleared_access_units == 3,
            "start must defensively clear prior resource residue");

    const auto idle_stop = controller.stop_publishing();
    require(idle_stop.has_value(), "stop while idle must be idempotent");
    const auto restarted = controller.start_publishing();
    require(restarted && restarted->session_id == 2,
            "controller must support a second session after normal stop");
    require(controller.stop_publishing().has_value(),
            "second publisher session must stop normally");
}

void output_and_capture_start_failures_rollback_exact_scope() {
    {
        Fixture fixture;
        fixture.output.set_start_issue({
            domain::VideoOutputWorkerOperation::OpenBackend,
            std::nullopt,
            "scripted output open failure",
        });
        app::DefaultPublisherController controller{
            fixture.plan(), fixture.pipeline(), fixture.notifier};

        const auto started = controller.start_publishing();
        require(!started &&
                    started.error().operation ==
                        app::PublisherControllerOperation::StartOutput,
                "output startup failure must retain its controller stage");
        const auto calls = call_names(fixture.calls);
        require(std::ranges::find(calls, "encoder.start") == calls.end() &&
                    std::ranges::find(calls, "capture.start") == calls.end() &&
                    std::ranges::find(calls, "output.abort") == calls.end(),
                "output startup failure must not control an unstarted worker");
    }

    {
        Fixture fixture;
        fixture.capture.set_start_issue({
            domain::VideoCaptureWorkerOperation::OpenBackend,
            std::nullopt,
            "scripted capture open failure",
        });
        app::DefaultPublisherController controller{
            fixture.plan(), fixture.pipeline(), fixture.notifier};

        const auto started = controller.start_publishing();
        require(!started &&
                    started.error().operation ==
                        app::PublisherControllerOperation::StartCapture,
                "capture startup failure must retain its controller stage");
        const auto calls = call_names(fixture.calls);
        require(call_index(calls, "capture.start") <
                    call_index(calls, "encoder.abort") &&
                    call_index(calls, "encoder.abort") <
                        call_index(calls, "output.abort"),
                "capture startup failure must abort started stages in reverse order");
        require(std::ranges::find(calls, "capture.stop") == calls.end(),
                "a capture worker that did not start must not be stopped");
    }
}

void encoder_start_failure_rolls_back_started_output() {
    Fixture fixture{1, 2};
    fixture.encoder.set_start_issue(encoder_failure());
    app::DefaultPublisherController controller{
        fixture.plan(), fixture.pipeline(), fixture.notifier};

    const auto started = controller.start_publishing();
    require(!started && started.error().operation ==
                            app::PublisherControllerOperation::StartEncoder,
            "encoder startup failure must retain its controller stage");
    require(controller.state() == app::PublisherControllerState::Idle,
            "startup rollback must return to idle");

    const auto calls = call_names(fixture.calls);
    require(call_index(calls, "output.start") <
                call_index(calls, "encoder.start") &&
                call_index(calls, "encoder.start") <
                    call_index(calls, "output.abort"),
            "encoder startup failure must abort the started output");
    require(std::ranges::find(calls, "capture.start") == calls.end(),
            "capture must not start after encoder startup failure");
    const auto stats = controller.stats();
    require(stats.start_failures == 1 && stats.started_sessions == 0,
            "startup failure must be counted without a started session");
}

void runtime_failure_aborts_and_requires_acknowledgement() {
    Fixture fixture{4, 3};
    app::DefaultPublisherController controller{
        fixture.plan(), fixture.pipeline(), fixture.notifier};
    require(controller.start_publishing().has_value(),
            "publisher must start before runtime failure");

    const auto event_thread = std::this_thread::get_id();
    fixture.output.fail(output_failure());
    const auto terminal = controller.wait_for_terminal_for(2s);
    require(terminal.status == app::PublisherWaitStatus::Failed &&
                terminal.issue &&
                terminal.issue->operation ==
                    app::PublisherControllerOperation::VideoOutputFailed &&
                std::holds_alternative<domain::VideoOutputWorkerIssue>(
                    terminal.issue->detail),
            "output failure must become the terminal controller issue");

    const auto calls = fixture.calls.snapshot();
    const auto abort = std::ranges::find_if(
        calls, [](const RecordedCall& call) {
            return call.name == "output.abort";
        });
    require(abort != calls.end() && abort->thread_id != event_thread,
            "failure callback must defer abort work to the controller thread");
    require(controller.stats().failed_sessions == 1 &&
                controller.stats().frame_store_size == 0 &&
                controller.stats().access_unit_queue_size == 0,
            "runtime failure must abort and clear both resources");

    const auto acknowledged = controller.stop_publishing();
    require(acknowledged.has_value() &&
                controller.state() == app::PublisherControllerState::Idle,
            "stop must acknowledge a terminal failure and return to idle");
    require(controller.start_publishing().has_value(),
            "an acknowledged failure must allow a new publisher session");
    require(controller.stop_publishing().has_value(),
            "session after failure acknowledgement must stop normally");
}

void output_failure_interrupts_encoder_drain() {
    Fixture fixture;
    fixture.encoder.block_drain();
    app::DefaultPublisherController controller{
        fixture.plan(), fixture.pipeline(), fixture.notifier};
    require(controller.start_publishing().has_value(),
            "publisher must start before drain interruption test");

    auto stopping = std::async(std::launch::async, [&controller] {
        return controller.stop_publishing();
    });
    require(fixture.encoder.wait_for_drain(),
            "normal stop must enter encoder drain");

    fixture.output.fail(output_failure());
    require(stopping.wait_for(2s) == std::future_status::ready,
            "output failure must interrupt a blocked encoder drain");
    const auto stopped = stopping.get();
    require(!stopped && controller.state() ==
                            app::PublisherControllerState::Failed,
            "interrupted normal stop must finish as a failed session");

    const auto calls = call_names(fixture.calls);
    require(call_index(calls, "encoder.drain") <
                call_index(calls, "encoder.abort"),
            "failure handling must abort an in-progress encoder drain");
    require(controller.stop_publishing().has_value(),
            "failed drain must be acknowledgeable");
}

void first_runtime_failure_is_preserved() {
    Fixture fixture;
    app::DefaultPublisherController controller{
        fixture.plan(), fixture.pipeline(), fixture.notifier};
    require(controller.start_publishing().has_value(),
            "publisher must start before first-failure test");

    fixture.encoder.fail(encoder_failure());
    fixture.output.fail(output_failure());
    const auto terminal = controller.wait_for_terminal_for(2s);
    require(terminal.status == app::PublisherWaitStatus::Failed &&
                terminal.issue &&
                terminal.issue->operation ==
                    app::PublisherControllerOperation::VideoEncoderFailed &&
                std::holds_alternative<domain::VideoEncoderWorkerIssue>(
                    terminal.issue->detail),
            "the first runtime failure must remain the primary issue");
    require(controller.stats().failed_sessions == 1,
            "multiple worker failures must count one failed session");
    require(controller.stop_publishing().has_value(),
            "first-failure session must be acknowledgeable");
}

void destructor_aborts_and_unsubscribes() {
    Fixture fixture{2, 2};
    {
        app::DefaultPublisherController controller{
            fixture.plan(), fixture.pipeline(), fixture.notifier};
        require(controller.start_publishing().has_value(),
                "publisher must start before destructor fallback test");
    }

    auto calls = call_names(fixture.calls);
    require(call_index(calls, "capture.stop") <
                call_index(calls, "encoder.abort") &&
                call_index(calls, "encoder.abort") <
                    call_index(calls, "output.abort"),
            "controller destructor must abort the active pipeline in order");
    require(fixture.frame_store.size() == 0 &&
                fixture.access_unit_queue.size() == 0,
            "controller destructor must clear controlled resources");

    const auto call_count = calls.size();
    fixture.output.fail(output_failure());
    calls = call_names(fixture.calls);
    require(calls.size() == call_count,
            "destroyed controller must not receive later worker failures");
}

}  // namespace

int main() {
    try {
        normal_lifecycle_is_ordered_and_repeatable();
        output_and_capture_start_failures_rollback_exact_scope();
        encoder_start_failure_rolls_back_started_output();
        runtime_failure_aborts_and_requires_acknowledgement();
        output_failure_interrupts_encoder_drain();
        first_runtime_failure_is_preserved();
        destructor_aborts_and_unsubscribes();
    } catch (const std::exception& error) {
        std::cerr << "publisher controller test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher controller tests passed\n";
    return EXIT_SUCCESS;
}
