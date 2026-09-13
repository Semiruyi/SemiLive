#include <semilive/receiver/infrastructure/output/file/h264_file_output_backend.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

namespace semilive::receiver::infra::output {
namespace {

using contracts::output::LiveVideoOutputIssue;
using contracts::output::LiveVideoOutputOperation;

[[nodiscard]] LiveVideoOutputIssue issue(
    const LiveVideoOutputOperation operation,
    const std::int64_t native_code,
    std::string message) {
    return {operation, native_code, std::move(message)};
}

[[nodiscard]] std::int64_t current_native_error() noexcept {
    return static_cast<std::int64_t>(errno);
}

}  // namespace

struct H264FileOutputBackend::Impl {
    enum class State : std::uint8_t {
        Closed,
        Open,
        Failed,
    };

    explicit Impl(std::filesystem::path output_path)
        : output_path_{std::move(output_path)} {}

    [[nodiscard]] contracts::output::LiveVideoOutputOpenResult open();
    [[nodiscard]] contracts::output::LiveVideoOutputSubmitResult submit(
        model::TimedH264AccessUnit access_unit);
    void close(contracts::output::LiveVideoOutputCloseMode mode) noexcept;

    std::filesystem::path output_path_;
    std::ofstream file_;
    State state_ = State::Closed;
};

contracts::output::LiveVideoOutputOpenResult
H264FileOutputBackend::Impl::open() {
    if (state_ != State::Closed) {
        return std::unexpected{issue(
            LiveVideoOutputOperation::State, 0,
            "H.264 file output backend can only open from the closed state")};
    }
    if (output_path_.empty()) {
        return std::unexpected{issue(LiveVideoOutputOperation::Open, 0,
                                     "H.264 output path must not be empty")};
    }

    errno = 0;
    file_.open(output_path_, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!file_.is_open()) {
        const auto native_code = current_native_error();
        file_.clear();
        return std::unexpected{issue(
            LiveVideoOutputOperation::Open, native_code,
            "failed to open H.264 output file: " + output_path_.string())};
    }

    state_ = State::Open;
    return contracts::output::LiveVideoOutputInfo{output_path_.string()};
}

contracts::output::LiveVideoOutputSubmitResult
H264FileOutputBackend::Impl::submit(model::TimedH264AccessUnit access_unit) {
    if (state_ != State::Open) {
        return std::unexpected{issue(
            LiveVideoOutputOperation::State, 0,
            "H.264 file output backend must be open before submitting")};
    }

    const auto annex_b = access_unit.access_unit().annex_b();
    if (annex_b.empty()) {
        state_ = State::Failed;
        return std::unexpected{issue(LiveVideoOutputOperation::Submit, 0,
                                     "H.264 access unit must not be empty")};
    }
    if (annex_b.size() >
        static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        state_ = State::Failed;
        return std::unexpected{issue(
            LiveVideoOutputOperation::Submit, 0,
            "H.264 access unit exceeds the file stream size limit")};
    }

    errno = 0;
    file_.write(reinterpret_cast<const char*>(annex_b.data()),
                static_cast<std::streamsize>(annex_b.size()));
    if (!file_) {
        const auto native_code = current_native_error();
        state_ = State::Failed;
        return std::unexpected{issue(LiveVideoOutputOperation::Submit,
                                     native_code,
                                     "failed to write H.264 access unit")};
    }

    return contracts::output::LiveVideoSubmitReceipt{
        contracts::output::LiveVideoSubmitStatus::Accepted, annex_b.size()};
}

void H264FileOutputBackend::Impl::close(
    const contracts::output::LiveVideoOutputCloseMode mode) noexcept {
    if (file_.is_open()) {
        if (mode == contracts::output::LiveVideoOutputCloseMode::Normal &&
            state_ == State::Open) {
            file_.flush();
        }
        file_.close();
    }
    file_.clear();
    state_ = State::Closed;
}

H264FileOutputBackend::H264FileOutputBackend(
    std::filesystem::path output_path)
    : impl_{std::make_unique<Impl>(std::move(output_path))} {}

H264FileOutputBackend::~H264FileOutputBackend() {
    impl_->close(contracts::output::LiveVideoOutputCloseMode::Abort);
}

contracts::output::LiveVideoOutputOpenResult H264FileOutputBackend::open() {
    return impl_->open();
}

contracts::output::LiveVideoOutputSubmitResult H264FileOutputBackend::submit(
    model::TimedH264AccessUnit access_unit) {
    return impl_->submit(std::move(access_unit));
}

void H264FileOutputBackend::close(
    const contracts::output::LiveVideoOutputCloseMode mode) noexcept {
    impl_->close(mode);
}

}  // namespace semilive::receiver::infra::output
