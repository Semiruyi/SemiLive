#include <semilive/receiver/composition/receiver_composition.hpp>

#include <semilive/receiver/application/default_receiver_controller.hpp>
#include <semilive/receiver/domain/worker/default_video_receive_worker.hpp>

#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace semilive::receiver::composition {
namespace {

ReceiverCompositionIssue make_issue(
    const ReceiverCompositionOperation operation,
    std::string message) {
    return {operation, std::nullopt, std::move(message)};
}

ReceiverCompositionIssue make_controller_issue(
    application::ReceiverControllerIssue issue) {
    auto message = issue.message;
    return {ReceiverCompositionOperation::StopSession,
            std::move(issue),
            std::move(message)};
}

}  // namespace

struct ReceiverComposition::Impl {
    enum class State {
        Unassembled,
        Assembled,
        Disposed,
    };

    explicit Impl(ReceiverConfig config) : config_{std::move(config)} {}

    ~Impl() {
        dispose_noexcept();
    }

    [[nodiscard]] ReceiverCompositionResult assemble();
    [[nodiscard]] application::ReceiverController* controller() noexcept;
    [[nodiscard]] const application::ReceiverController*
    controller() const noexcept;
    [[nodiscard]] ReceiverCompositionResult dispose();

    void reset_graph() noexcept;
    void dispose_noexcept() noexcept;

    [[maybe_unused]] ReceiverConfig config_;
    State state_ = State::Unassembled;
    std::unique_ptr<domain::DefaultVideoReceiveWorker> worker_;
    std::unique_ptr<application::DefaultReceiverController> controller_;
};

ReceiverCompositionResult ReceiverComposition::Impl::assemble() {
    if (state_ == State::Assembled) {
        return std::unexpected{make_issue(
            ReceiverCompositionOperation::Control,
            "receiver composition is already assembled")};
    }
    if (state_ == State::Disposed) {
        return std::unexpected{make_issue(
            ReceiverCompositionOperation::Control,
            "disposed receiver composition cannot be assembled")};
    }

    auto operation = ReceiverCompositionOperation::CreateWorker;
    try {
        worker_ = std::make_unique<domain::DefaultVideoReceiveWorker>();

        operation = ReceiverCompositionOperation::CreateController;
        controller_ =
            std::make_unique<application::DefaultReceiverController>(
                *worker_);
        state_ = State::Assembled;
        return {};
    } catch (const std::exception& error) {
        reset_graph();
        return std::unexpected{make_issue(operation, error.what())};
    } catch (...) {
        reset_graph();
        return std::unexpected{make_issue(
            operation, "unknown exception while assembling receiver")};
    }
}

application::ReceiverController*
ReceiverComposition::Impl::controller() noexcept {
    if (state_ != State::Assembled) {
        return nullptr;
    }
    return controller_.get();
}

const application::ReceiverController*
ReceiverComposition::Impl::controller() const noexcept {
    if (state_ != State::Assembled) {
        return nullptr;
    }
    return controller_.get();
}

ReceiverCompositionResult ReceiverComposition::Impl::dispose() {
    if (state_ == State::Disposed) {
        return {};
    }

    state_ = State::Disposed;
    std::optional<ReceiverCompositionIssue> issue;
    if (controller_ &&
        controller_->state() != application::ReceiverControllerState::Idle) {
        try {
            auto stopped = controller_->stop_receiving();
            if (!stopped) {
                issue = make_controller_issue(std::move(stopped.error()));
            }
        } catch (const std::exception& error) {
            issue = make_issue(ReceiverCompositionOperation::StopSession,
                               error.what());
        } catch (...) {
            issue = make_issue(
                ReceiverCompositionOperation::StopSession,
                "unknown exception while stopping receiver during disposal");
        }
    }

    reset_graph();
    if (issue) {
        return std::unexpected{std::move(*issue)};
    }
    return {};
}

void ReceiverComposition::Impl::reset_graph() noexcept {
    controller_.reset();
    worker_.reset();
}

void ReceiverComposition::Impl::dispose_noexcept() noexcept {
    try {
        (void)dispose();
    } catch (...) {
        state_ = State::Disposed;
        reset_graph();
    }
}

ReceiverComposition::ReceiverComposition(ReceiverConfig config)
    : impl_{std::make_unique<Impl>(std::move(config))} {}

ReceiverComposition::~ReceiverComposition() = default;

ReceiverCompositionResult ReceiverComposition::assemble() {
    return impl_->assemble();
}

application::ReceiverController*
ReceiverComposition::controller() noexcept {
    return impl_->controller();
}

const application::ReceiverController*
ReceiverComposition::controller() const noexcept {
    return impl_->controller();
}

ReceiverCompositionResult ReceiverComposition::dispose() {
    return impl_->dispose();
}

}  // namespace semilive::receiver::composition
