#include <semilive/receiver/domain/worker/default_video_receive_worker.hpp>

#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

namespace semilive::receiver::domain {
namespace {

namespace input_contract = contracts::network;
namespace output_contract = contracts::output;

[[nodiscard]] VideoReceiveWorkerIssue make_issue(
    const VideoReceiveWorkerOperation operation,
    std::string message) {
    return {operation, std::nullopt, std::nullopt, std::move(message)};
}

[[nodiscard]] VideoReceiveWorkerIssue make_input_issue(
    const VideoReceiveWorkerOperation operation,
    input_contract::DatagramSourceIssue issue) {
    auto message = issue.message;
    return {operation, std::move(issue), std::nullopt, std::move(message)};
}

[[nodiscard]] VideoReceiveWorkerIssue make_output_issue(
    const VideoReceiveWorkerOperation operation,
    output_contract::LiveVideoOutputIssue issue) {
    auto message = issue.message;
    return {operation, std::nullopt, std::move(issue), std::move(message)};
}

}  // namespace

DefaultVideoReceiveWorkerConfigValidationResult
validate_default_video_receive_worker_config(
    const DefaultVideoReceiveWorkerConfig& config) {
    if (config.receive_poll_interval <= std::chrono::milliseconds::zero()) {
        return std::unexpected{
            "video receive poll interval must be positive"};
    }
    return {};
}

struct DefaultVideoReceiveWorker::Impl {
    Impl(DefaultVideoReceiveWorkerConfig config,
         std::unique_ptr<input_contract::DatagramSourceBackend> input,
         std::unique_ptr<H264RtpReceivePipeline> pipeline,
         std::unique_ptr<output_contract::LiveVideoOutputBackend> output)
        : config_{std::move(config)},
          input_{std::move(input)},
          pipeline_{std::move(pipeline)},
          output_{std::move(output)} {
        if (!input_ || !pipeline_ || !output_) {
            throw std::invalid_argument{
                "video receive worker dependencies must not be null"};
        }
        if (const auto valid =
                validate_default_video_receive_worker_config(config_);
            !valid) {
            throw std::invalid_argument{valid.error()};
        }
    }

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
    [[nodiscard]] bool start_session(std::stop_token stop_token);
    [[nodiscard]] bool process_one_observation(std::stop_token stop_token);
    [[nodiscard]] std::optional<VideoReceiveWorkerIssue> submit_outputs(
        H264RtpReceivePipelineOutputs outputs);
    [[nodiscard]] bool finish_start_success(
        input_contract::DatagramSourceInfo input_info,
        output_contract::LiveVideoOutputInfo output_info);
    void finish_start_failure(VideoReceiveWorkerIssue issue) noexcept;
    void fail_running_session(VideoReceiveWorkerIssue issue) noexcept;
    void publish_pipeline_stats() noexcept;
    void close_session(output_contract::LiveVideoOutputCloseMode mode) noexcept;
    void stop_noexcept() noexcept;

    DefaultVideoReceiveWorkerConfig config_;
    std::unique_ptr<input_contract::DatagramSourceBackend> input_;
    std::unique_ptr<H264RtpReceivePipeline> pipeline_;
    std::unique_ptr<output_contract::LiveVideoOutputBackend> output_;
    bool input_open_ = false;
    bool output_open_ = false;

    mutable std::mutex mutex_;
    std::condition_variable state_changed_;
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

    if (thread_.joinable()) {
        lock.unlock();
        thread_.join();
        lock.lock();
    }

    stats_.state = VideoReceiveWorkerState::Starting;
    ++stats_.session_id;
    stats_.received_datagrams = 0;
    stats_.receive_timeouts = 0;
    stats_.submitted_access_units = 0;
    stats_.submitted_bytes = 0;
    stats_.backpressure_drops = 0;
    stats_.discarded_after_backpressure = 0;
    stats_.input.reset();
    stats_.output.reset();
    stats_.pipeline = {};
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
    if (stats_.state == VideoReceiveWorkerState::Running) {
        return VideoReceiveStarted{stats_.session_id};
    }

    const auto issue = last_issue_.value_or(make_issue(
        VideoReceiveWorkerOperation::Control,
        "video receive session was interrupted while starting"));
    const auto join_finished_start =
        stats_.state == VideoReceiveWorkerState::Idle;
    lock.unlock();
    if (join_finished_start && thread_.joinable()) {
        thread_.join();
    }
    return std::unexpected{issue};
}

VideoReceiveStopResult DefaultVideoReceiveWorker::Impl::stop() {
    std::unique_lock lock{mutex_};
    if (stats_.state == VideoReceiveWorkerState::Idle) {
        const auto session_id = stats_.session_id;
        const auto join_finished = thread_.joinable();
        lock.unlock();
        if (join_finished) {
            thread_.join();
        }
        return VideoReceiveStopped{session_id};
    }

    const auto stopped_session = stats_.session_id;
    const auto stopped_state = stats_.state;
    stats_.state = VideoReceiveWorkerState::Stopping;
    thread_.request_stop();
    state_changed_.notify_all();
    lock.unlock();

    if (thread_.joinable()) {
        thread_.join();
    }

    lock.lock();
    stats_.state = VideoReceiveWorkerState::Idle;
    if (stopped_state == VideoReceiveWorkerState::Running) {
        ++stats_.completed_sessions;
    }
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
    bool session_started = false;
    try {
        if (!start_session(stop_token)) {
            return;
        }
        session_started = true;

        while (!stop_token.stop_requested()) {
            if (!process_one_observation(stop_token)) {
                return;
            }
        }

        close_session(output_contract::LiveVideoOutputCloseMode::Normal);
    } catch (const std::exception& error) {
        if (session_started) {
            fail_running_session(make_issue(
                VideoReceiveWorkerOperation::Internal, error.what()));
        } else {
            close_session(output_contract::LiveVideoOutputCloseMode::Abort);
            finish_start_failure(make_issue(
                VideoReceiveWorkerOperation::StartSession, error.what()));
        }
    } catch (...) {
        if (session_started) {
            fail_running_session(make_issue(
                VideoReceiveWorkerOperation::Internal,
                "unknown exception in video receive session"));
        } else {
            close_session(output_contract::LiveVideoOutputCloseMode::Abort);
            finish_start_failure(make_issue(
                VideoReceiveWorkerOperation::StartSession,
                "unknown exception while starting video receive session"));
        }
    }
}

bool DefaultVideoReceiveWorker::Impl::start_session(
    const std::stop_token stop_token) {
    pipeline_->reset();

    output_contract::LiveVideoOutputOpenResult opened_output;
    try {
        opened_output = output_->open();
    } catch (const std::exception& error) {
        output_->close(output_contract::LiveVideoOutputCloseMode::Abort);
        finish_start_failure(make_issue(VideoReceiveWorkerOperation::OpenOutput,
                                        error.what()));
        return false;
    } catch (...) {
        output_->close(output_contract::LiveVideoOutputCloseMode::Abort);
        finish_start_failure(make_issue(
            VideoReceiveWorkerOperation::OpenOutput,
            "video output open threw an unknown exception"));
        return false;
    }
    if (!opened_output) {
        auto issue = make_output_issue(VideoReceiveWorkerOperation::OpenOutput,
                                       std::move(opened_output.error()));
        output_->close(output_contract::LiveVideoOutputCloseMode::Abort);
        finish_start_failure(std::move(issue));
        return false;
    }
    output_open_ = true;

    input_contract::DatagramSourceOpenResult opened_input;
    try {
        opened_input = input_->open(config_.input);
    } catch (const std::exception& error) {
        input_->close();
        close_session(output_contract::LiveVideoOutputCloseMode::Abort);
        finish_start_failure(make_issue(VideoReceiveWorkerOperation::OpenInput,
                                        error.what()));
        return false;
    } catch (...) {
        input_->close();
        close_session(output_contract::LiveVideoOutputCloseMode::Abort);
        finish_start_failure(make_issue(
            VideoReceiveWorkerOperation::OpenInput,
            "datagram input open threw an unknown exception"));
        return false;
    }
    if (!opened_input) {
        auto issue = make_input_issue(VideoReceiveWorkerOperation::OpenInput,
                                      std::move(opened_input.error()));
        input_->close();
        close_session(output_contract::LiveVideoOutputCloseMode::Abort);
        finish_start_failure(std::move(issue));
        return false;
    }
    input_open_ = true;

    if (stop_token.stop_requested()) {
        close_session(output_contract::LiveVideoOutputCloseMode::Abort);
        finish_start_failure(make_issue(
            VideoReceiveWorkerOperation::Control,
            "video receive session was stopped while starting"));
        return false;
    }

    if (!finish_start_success(std::move(*opened_input),
                              std::move(*opened_output))) {
        close_session(output_contract::LiveVideoOutputCloseMode::Abort);
        return false;
    }
    return true;
}

bool DefaultVideoReceiveWorker::Impl::process_one_observation(
    const std::stop_token stop_token) {
    input_contract::DatagramSourceReceiveResult received;
    try {
        received = input_->receive_for(config_.receive_poll_interval);
    } catch (const std::exception& error) {
        fail_running_session(make_issue(
            VideoReceiveWorkerOperation::ReceiveInput, error.what()));
        return false;
    } catch (...) {
        fail_running_session(make_issue(
            VideoReceiveWorkerOperation::ReceiveInput,
            "datagram receive threw an unknown exception"));
        return false;
    }
    if (stop_token.stop_requested()) {
        return true;
    }
    if (!received) {
        fail_running_session(make_input_issue(
            VideoReceiveWorkerOperation::ReceiveInput,
            std::move(received.error())));
        return false;
    }

    H264RtpReceivePipelineOutputs outputs;
    try {
        if (auto* datagram = std::get_if<model::UdpDatagram>(&*received)) {
            {
                std::lock_guard lock{mutex_};
                ++stats_.received_datagrams;
            }
            outputs = pipeline_->push(std::move(*datagram));
        } else {
            {
                std::lock_guard lock{mutex_};
                ++stats_.receive_timeouts;
            }
            outputs = pipeline_->poll(H264RtpReceivePipeline::Clock::now());
        }
    } catch (const std::exception& error) {
        fail_running_session(make_issue(VideoReceiveWorkerOperation::RunSession,
                                        error.what()));
        return false;
    } catch (...) {
        fail_running_session(make_issue(
            VideoReceiveWorkerOperation::RunSession,
            "receive pipeline threw an unknown exception"));
        return false;
    }

    publish_pipeline_stats();
    if (auto issue = submit_outputs(std::move(outputs))) {
        fail_running_session(std::move(*issue));
        return false;
    }
    return true;
}

std::optional<VideoReceiveWorkerIssue>
DefaultVideoReceiveWorker::Impl::submit_outputs(
    H264RtpReceivePipelineOutputs outputs) {
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        const auto bytes = outputs[index].access_unit().annex_b().size();
        output_contract::LiveVideoOutputSubmitResult submitted;
        try {
            submitted = output_->submit(std::move(outputs[index]));
        } catch (const std::exception& error) {
            return make_issue(VideoReceiveWorkerOperation::SubmitOutput,
                              error.what());
        } catch (...) {
            return make_issue(
                VideoReceiveWorkerOperation::SubmitOutput,
                "video output submit threw an unknown exception");
        }
        if (!submitted) {
            return make_output_issue(VideoReceiveWorkerOperation::SubmitOutput,
                                     std::move(submitted.error()));
        }

        if (submitted->status ==
            output_contract::LiveVideoSubmitStatus::DroppedBackpressure) {
            pipeline_->require_random_access();
            {
                std::lock_guard lock{mutex_};
                ++stats_.backpressure_drops;
                stats_.discarded_after_backpressure +=
                    outputs.size() - index - 1U;
            }
            publish_pipeline_stats();
            return std::nullopt;
        }

        if (submitted->accepted_bytes != bytes) {
            return make_issue(
                VideoReceiveWorkerOperation::SubmitOutput,
                "video output accepted a partial H.264 access unit");
        }
        std::lock_guard lock{mutex_};
        ++stats_.submitted_access_units;
        stats_.submitted_bytes += submitted->accepted_bytes;
    }
    return std::nullopt;
}

bool DefaultVideoReceiveWorker::Impl::finish_start_success(
    input_contract::DatagramSourceInfo input_info,
    output_contract::LiveVideoOutputInfo output_info) {
    std::lock_guard lock{mutex_};
    if (stats_.state != VideoReceiveWorkerState::Starting) {
        return false;
    }
    stats_.input = std::move(input_info);
    stats_.output = std::move(output_info);
    stats_.state = VideoReceiveWorkerState::Running;
    ++stats_.started_sessions;
    state_changed_.notify_all();
    return true;
}

void DefaultVideoReceiveWorker::Impl::finish_start_failure(
    VideoReceiveWorkerIssue issue) noexcept {
    std::lock_guard lock{mutex_};
    if (stats_.state != VideoReceiveWorkerState::Stopping) {
        stats_.state = VideoReceiveWorkerState::Idle;
    }
    ++stats_.start_failures;
    last_issue_ = std::move(issue);
    state_changed_.notify_all();
}

void DefaultVideoReceiveWorker::Impl::fail_running_session(
    VideoReceiveWorkerIssue issue) noexcept {
    close_session(output_contract::LiveVideoOutputCloseMode::Abort);
    publish_pipeline_stats();

    std::lock_guard lock{mutex_};
    if (stats_.state == VideoReceiveWorkerState::Stopping) {
        return;
    }
    stats_.state = VideoReceiveWorkerState::Failed;
    ++stats_.failed_sessions;
    last_issue_ = std::move(issue);
    state_changed_.notify_all();
}

void DefaultVideoReceiveWorker::Impl::publish_pipeline_stats() noexcept {
    const auto pipeline_stats = pipeline_->stats();
    std::lock_guard lock{mutex_};
    stats_.pipeline = pipeline_stats;
}

void DefaultVideoReceiveWorker::Impl::close_session(
    const output_contract::LiveVideoOutputCloseMode mode) noexcept {
    if (input_open_) {
        input_->close();
        input_open_ = false;
    }
    if (output_open_) {
        output_->close(mode);
        output_open_ = false;
    }
}

void DefaultVideoReceiveWorker::Impl::stop_noexcept() noexcept {
    try {
        (void)stop();
    } catch (...) {
        thread_.request_stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
}

DefaultVideoReceiveWorker::DefaultVideoReceiveWorker(
    DefaultVideoReceiveWorkerConfig config,
    std::unique_ptr<contracts::network::DatagramSourceBackend> input,
    std::unique_ptr<H264RtpReceivePipeline> pipeline,
    std::unique_ptr<contracts::output::LiveVideoOutputBackend> output)
    : impl_{std::make_unique<Impl>(std::move(config), std::move(input),
                                  std::move(pipeline), std::move(output))} {}

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
