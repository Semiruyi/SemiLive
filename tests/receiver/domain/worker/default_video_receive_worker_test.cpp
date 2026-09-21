#include <semilive/receiver/domain/worker/default_video_receive_worker.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <initializer_list>
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

namespace domain = semilive::receiver::domain;
namespace input_contract = semilive::receiver::contracts::network;
namespace model = semilive::receiver::model;
namespace output_contract = semilive::receiver::contracts::output;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

enum class BackendCallType : std::uint8_t {
    OutputOpen,
    InputOpen,
    InputClose,
    OutputSubmit,
    OutputClose,
};

struct BackendCall {
    BackendCallType type = BackendCallType::InputOpen;
    std::thread::id thread_id;
};

class BackendTrace final {
public:
    void record(const BackendCallType type) {
        std::lock_guard lock{mutex_};
        calls_.push_back({type, std::this_thread::get_id()});
    }

    [[nodiscard]] std::vector<BackendCall> calls() const {
        std::lock_guard lock{mutex_};
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<BackendCall> calls_;
};

class ScriptedDatagramSource final
    : public input_contract::DatagramSourceBackend {
public:
    explicit ScriptedDatagramSource(std::shared_ptr<BackendTrace> trace)
        : trace_{std::move(trace)} {}

    [[nodiscard]] input_contract::DatagramSourceOpenResult open(
        const input_contract::DatagramSourceConfig& config) override {
        trace_->record(BackendCallType::InputOpen);
        std::lock_guard lock{mutex_};
        if (!open_results_.empty()) {
            auto result = std::move(open_results_.front());
            open_results_.pop_front();
            return result;
        }
        return input_contract::DatagramSourceInfo{
            config.bind_address,
            config.bind_port == 0 ? static_cast<std::uint16_t>(40'000)
                                  : config.bind_port,
            config.maximum_datagram_bytes,
            config.receive_buffer_bytes};
    }

    [[nodiscard]] input_contract::DatagramSourceReceiveResult receive_for(
        const std::chrono::milliseconds timeout) override {
        std::unique_lock lock{mutex_};
        cv_.wait_for(lock, timeout,
                     [this] { return !observations_.empty(); });
        if (observations_.empty()) {
            return input_contract::DatagramSourceObservation{
                input_contract::DatagramReceiveTimeout{}};
        }
        auto result = std::move(observations_.front());
        observations_.pop_front();
        return result;
    }

    void close() noexcept override {
        trace_->record(BackendCallType::InputClose);
    }

    void queue_open_result(input_contract::DatagramSourceOpenResult result) {
        std::lock_guard lock{mutex_};
        open_results_.push_back(std::move(result));
    }

    void queue_observation(
        input_contract::DatagramSourceReceiveResult result) {
        {
            std::lock_guard lock{mutex_};
            observations_.push_back(std::move(result));
        }
        cv_.notify_one();
    }

private:
    std::shared_ptr<BackendTrace> trace_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<input_contract::DatagramSourceOpenResult> open_results_;
    std::deque<input_contract::DatagramSourceReceiveResult> observations_;
};

class ScriptedVideoOutput final
    : public output_contract::LiveVideoOutputBackend {
public:
    explicit ScriptedVideoOutput(std::shared_ptr<BackendTrace> trace)
        : trace_{std::move(trace)} {}

    [[nodiscard]] output_contract::LiveVideoOutputOpenResult open() override {
        trace_->record(BackendCallType::OutputOpen);
        std::lock_guard lock{mutex_};
        if (!open_results_.empty()) {
            auto result = std::move(open_results_.front());
            open_results_.pop_front();
            return result;
        }
        return output_contract::LiveVideoOutputInfo{"scripted-video-output"};
    }

    [[nodiscard]] output_contract::LiveVideoOutputSubmitResult submit(
        model::TimedH264AccessUnit access_unit) override {
        trace_->record(BackendCallType::OutputSubmit);
        std::lock_guard lock{mutex_};
        const auto bytes = access_unit.access_unit().annex_b().size();
        submitted_.push_back(std::move(access_unit));
        auto status = output_contract::LiveVideoSubmitStatus::Accepted;
        if (!statuses_.empty()) {
            status = statuses_.front();
            statuses_.pop_front();
        }
        return output_contract::LiveVideoSubmitReceipt{
            status,
            status == output_contract::LiveVideoSubmitStatus::Accepted
                ? bytes
                : 0U};
    }

    void close(const output_contract::LiveVideoOutputCloseMode mode) noexcept
        override {
        trace_->record(BackendCallType::OutputClose);
        std::lock_guard lock{mutex_};
        close_modes_.push_back(mode);
    }

    void queue_open_result(
        output_contract::LiveVideoOutputOpenResult result) {
        std::lock_guard lock{mutex_};
        open_results_.push_back(std::move(result));
    }

    void queue_status(const output_contract::LiveVideoSubmitStatus status) {
        std::lock_guard lock{mutex_};
        statuses_.push_back(status);
    }

    [[nodiscard]] std::vector<model::TimedH264AccessUnit> submitted() const {
        std::lock_guard lock{mutex_};
        return submitted_;
    }

    [[nodiscard]] std::vector<output_contract::LiveVideoOutputCloseMode>
    close_modes() const {
        std::lock_guard lock{mutex_};
        return close_modes_;
    }

private:
    std::shared_ptr<BackendTrace> trace_;
    mutable std::mutex mutex_;
    std::deque<output_contract::LiveVideoOutputOpenResult> open_results_;
    std::deque<output_contract::LiveVideoSubmitStatus> statuses_;
    std::vector<model::TimedH264AccessUnit> submitted_;
    std::vector<output_contract::LiveVideoOutputCloseMode> close_modes_;
};

class WorkerFixture final {
public:
    explicit WorkerFixture(
        const std::chrono::milliseconds output_stall_threshold = 100ms)
        : trace{std::make_shared<BackendTrace>()} {
        auto input_owner =
            std::make_unique<ScriptedDatagramSource>(trace);
        input = input_owner.get();
        auto output_owner = std::make_unique<ScriptedVideoOutput>(trace);
        output = output_owner.get();

        domain::DefaultVideoReceiveWorkerConfig config;
        config.input.bind_address = "127.0.0.1";
        config.input.bind_port = 0;
        config.receive_poll_interval = 2ms;
        config.output_stall_threshold = output_stall_threshold;
        worker = std::make_unique<domain::DefaultVideoReceiveWorker>(
            std::move(config), std::move(input_owner),
            std::make_unique<domain::H264RtpReceivePipeline>(),
            std::move(output_owner));
    }

    std::shared_ptr<BackendTrace> trace;
    ScriptedDatagramSource* input = nullptr;
    ScriptedVideoOutput* output = nullptr;
    std::unique_ptr<domain::DefaultVideoReceiveWorker> worker;
};

void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value >> 8U));
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::byte>(value >> 24U));
    bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
}

[[nodiscard]] model::UdpDatagram datagram(
    const std::uint16_t sequence,
    const std::uint32_t timestamp,
    const bool marker,
    const std::initializer_list<std::uint8_t> payload) {
    std::vector<std::byte> bytes;
    bytes.reserve(12U + payload.size());
    bytes.push_back(std::byte{0x80});
    bytes.push_back(static_cast<std::byte>(
        static_cast<std::uint8_t>((marker ? 0x80U : 0U) | 96U)));
    append_u16(bytes, sequence);
    append_u32(bytes, timestamp);
    append_u32(bytes, 0x1234'5678U);
    for (const auto value : payload) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return {std::move(bytes), model::UdpDatagram::Clock::now()};
}

void queue_datagram(ScriptedDatagramSource& input,
                    model::UdpDatagram value) {
    input.queue_observation(input_contract::DatagramSourceObservation{
        std::move(value)});
}

void queue_random_access(ScriptedDatagramSource& input,
                         const std::uint16_t first_sequence,
                         const std::uint32_t timestamp) {
    queue_datagram(input,
                   datagram(first_sequence, timestamp, false, {0x67, 0x11}));
    queue_datagram(
        input, datagram(static_cast<std::uint16_t>(first_sequence + 1U),
                        timestamp, false, {0x68, 0x22}));
    queue_datagram(
        input, datagram(static_cast<std::uint16_t>(first_sequence + 2U),
                        timestamp, true, {0x65, 0x33}));
}

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

void drives_the_complete_pipeline_on_one_worker_thread() {
    WorkerFixture fixture;
    const auto started = fixture.worker->start();
    require(started.has_value(), "worker session must start");

    queue_random_access(*fixture.input, 100, 90'000);
    queue_datagram(*fixture.input,
                   datagram(103, 93'000, true, {0x61, 0x44}));
    require(wait_until([&fixture] {
                return fixture.worker->stats().submitted_access_units == 2;
            }),
            "worker must submit the random-access and following delta AU");

    const auto running_stats = fixture.worker->stats();
    require(running_stats.received_datagrams == 4 &&
                running_stats.received_bytes > 0 &&
                running_stats.pipeline.output_access_units == 2 &&
                running_stats.submitted_access_units == 2,
            "worker must publish input, pipeline and output statistics");
    require(running_stats.input && running_stats.input->bound_port == 40'000 &&
                running_stats.output &&
                running_stats.output->output_name == "scripted-video-output",
            "worker must retain the active backend identities");

    const auto submitted = fixture.output->submitted();
    require(submitted.size() == 2 && submitted[0].discontinuity_before() &&
                !submitted[1].discontinuity_before(),
            "worker must preserve recovery metadata at the output boundary");
    require(fixture.worker->stop().has_value(),
            "running worker must stop normally");

    const auto close_modes = fixture.output->close_modes();
    require(close_modes.size() == 1 &&
                close_modes[0] ==
                    output_contract::LiveVideoOutputCloseMode::Normal,
            "normal stop must close output normally exactly once");
    const auto calls = fixture.trace->calls();
    require(calls.size() >= 6 &&
                calls[0].type == BackendCallType::OutputOpen &&
                calls[1].type == BackendCallType::InputOpen &&
                calls[calls.size() - 2].type == BackendCallType::InputClose &&
                calls.back().type == BackendCallType::OutputClose,
            "worker must open consumer first and close producer first");
    for (const auto& call : calls) {
        require(call.thread_id == calls.front().thread_id,
                "every backend operation must stay on one worker thread");
    }
    require(fixture.worker->stats().completed_sessions == 1,
            "normal stop must complete one session");
    const auto stopped_stats = fixture.worker->stats();
    require(stopped_stats.session_duration >
                std::chrono::nanoseconds::zero() &&
                stopped_stats.first_output_delay.has_value() &&
                stopped_stats.maximum_output_gap.has_value(),
            "worker must retain session and accepted-output timing metrics");
}

void backpressure_discards_until_the_next_random_access_point() {
    WorkerFixture fixture;
    fixture.output->queue_status(
        output_contract::LiveVideoSubmitStatus::Accepted);
    fixture.output->queue_status(
        output_contract::LiveVideoSubmitStatus::DroppedBackpressure);
    fixture.output->queue_status(
        output_contract::LiveVideoSubmitStatus::Accepted);
    require(fixture.worker->start().has_value(),
            "backpressure test session must start");

    queue_random_access(*fixture.input, 10, 90'000);
    queue_datagram(*fixture.input,
                   datagram(13, 93'000, true, {0x61, 0x40}));
    queue_datagram(*fixture.input,
                   datagram(14, 96'000, true, {0x61, 0x41}));
    queue_random_access(*fixture.input, 15, 99'000);

    require(wait_until([&fixture] {
                const auto stats = fixture.worker->stats();
                return stats.submitted_access_units == 2 &&
                       stats.backpressure_drops == 1;
            }),
            "worker must recover from an output backpressure drop");
    const auto submitted = fixture.output->submitted();
    require(submitted.size() == 3,
            "inter AU after backpressure must not reach the output");
    require(submitted[0].discontinuity_before() &&
                !submitted[1].discontinuity_before() &&
                submitted[2].discontinuity_before() &&
                submitted[2].access_unit().is_random_access_candidate(),
            "the next submitted AU after a drop must be a decoder refresh");
    const auto stats = fixture.worker->stats();
    require(stats.pipeline.recovery.external_discontinuities == 1 &&
                stats.pipeline.recovery.dropped_while_waiting == 1,
            "pipeline statistics must expose backpressure recovery");
    require(fixture.worker->stop().has_value(),
            "recovered worker must stop normally");
}

void session_end_reports_the_censored_terminal_gap_separately() {
    WorkerFixture fixture{1ms};
    require(fixture.worker->start().has_value(),
            "terminal stall test session must start");
    queue_random_access(*fixture.input, 200, 90'000);
    require(wait_until([&fixture] {
                return fixture.worker->stats().submitted_access_units == 1;
            }),
            "terminal stall test must produce an initial output");

    std::this_thread::sleep_for(10ms);
    require(fixture.worker->stop().has_value(),
            "terminal stall test session must stop");

    const auto stats = fixture.worker->stats();
    require(stats.terminal_output_gap.has_value() &&
                *stats.terminal_output_gap > 1ms &&
                !stats.maximum_output_gap.has_value() &&
                stats.output_stall_events == 0 &&
                stats.output_stall_excess_total ==
                    std::chrono::nanoseconds::zero(),
            "session end must not turn a censored terminal gap into a stall");
}

void input_open_failure_rolls_back_output_and_allows_retry() {
    WorkerFixture fixture;
    fixture.input->queue_open_result(std::unexpected{
        input_contract::DatagramSourceIssue{
            input_contract::DatagramSourceOperation::Open,
            98,
            "scripted input bind failed"}});

    const auto failed = fixture.worker->start();
    require(!failed &&
                failed.error().operation ==
                    domain::VideoReceiveWorkerOperation::OpenInput &&
                failed.error().input_issue &&
                failed.error().input_issue->native_code == 98,
            "input open failure must retain its structured issue");
    require(fixture.worker->state() == domain::VideoReceiveWorkerState::Idle &&
                fixture.worker->stats().start_failures == 1,
            "start failure must restore idle and increment statistics");
    const auto first_close_modes = fixture.output->close_modes();
    require(first_close_modes.size() == 1 &&
                first_close_modes[0] ==
                    output_contract::LiveVideoOutputCloseMode::Abort,
            "input open failure must abort the already opened output");

    const auto retry = fixture.worker->start();
    require(retry && retry->session_id == 2,
            "worker must start a fresh session after rollback");
    require(fixture.worker->stop().has_value(),
            "retried session must stop normally");
    const auto stats = fixture.worker->stats();
    require(stats.started_sessions == 1 && stats.start_failures == 1,
            "retry statistics must distinguish starts from start failures");
}

void output_open_failure_never_starts_the_input() {
    WorkerFixture fixture;
    fixture.output->queue_open_result(std::unexpected{
        output_contract::LiveVideoOutputIssue{
            output_contract::LiveVideoOutputOperation::Open,
            13,
            "scripted output open failed"}});

    const auto failed = fixture.worker->start();
    require(!failed &&
                failed.error().operation ==
                    domain::VideoReceiveWorkerOperation::OpenOutput &&
                failed.error().output_issue &&
                failed.error().output_issue->native_code == 13,
            "output open failure must retain its structured issue");
    const auto calls = fixture.trace->calls();
    require(calls.size() == 2 &&
                calls[0].type == BackendCallType::OutputOpen &&
                calls[1].type == BackendCallType::OutputClose,
            "output open failure must not attempt to open the input");
    const auto close_modes = fixture.output->close_modes();
    require(close_modes.size() == 1 &&
                close_modes[0] ==
                    output_contract::LiveVideoOutputCloseMode::Abort,
            "failed output open must still receive abort cleanup");
}

void runtime_input_failure_aborts_and_requires_stop_acknowledgement() {
    WorkerFixture fixture;
    require(fixture.worker->start().has_value(),
            "runtime failure test session must start");
    fixture.input->queue_observation(std::unexpected{
        input_contract::DatagramSourceIssue{
            input_contract::DatagramSourceOperation::Receive,
            10054,
            "scripted receive failed"}});

    require(wait_until([&fixture] {
                return fixture.worker->state() ==
                       domain::VideoReceiveWorkerState::Failed;
            }),
            "runtime input failure must terminate the session");
    const auto terminal = fixture.worker->wait_for_terminal_for(10ms);
    require(terminal.status == domain::VideoReceiveWaitStatus::Failed &&
                terminal.issue &&
                terminal.issue->operation ==
                    domain::VideoReceiveWorkerOperation::ReceiveInput &&
                terminal.issue->input_issue &&
                terminal.issue->input_issue->native_code == 10054,
            "terminal wait must expose the runtime input failure");
    const auto close_modes = fixture.output->close_modes();
    require(close_modes.size() == 1 &&
                close_modes[0] ==
                    output_contract::LiveVideoOutputCloseMode::Abort,
            "runtime failure must abort the output session");
    require(fixture.worker->stats().failed_sessions == 1,
            "runtime failure must increment failed session statistics");

    const auto restart_while_failed = fixture.worker->start();
    require(!restart_while_failed &&
                restart_while_failed.error().operation ==
                    domain::VideoReceiveWorkerOperation::Control,
            "failed worker must require explicit stop acknowledgement");
    require(fixture.worker->stop().has_value() &&
                fixture.worker->state() == domain::VideoReceiveWorkerState::Idle,
            "stop must acknowledge failure and restore idle state");
}

}  // namespace

int main() {
    try {
        drives_the_complete_pipeline_on_one_worker_thread();
        backpressure_discards_until_the_next_random_access_point();
        session_end_reports_the_censored_terminal_gap_separately();
        input_open_failure_rolls_back_output_and_allows_retry();
        output_open_failure_never_starts_the_input();
        runtime_input_failure_aborts_and_requires_stop_acknowledgement();
    } catch (const std::exception& error) {
        std::cerr << "default video receive worker test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "default video receive worker tests passed\n";
    return EXIT_SUCCESS;
}
