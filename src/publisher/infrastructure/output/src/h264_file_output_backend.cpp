#include <semilive/publisher/infrastructure/output/h264_file_output_backend.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

namespace semilive::publisher::infra::output {
namespace {

using contracts::output::VideoOutputIssue;
using contracts::output::VideoOutputOperation;

VideoOutputIssue issue(const VideoOutputOperation operation,
                       const std::int64_t native_code,
                       std::string message) {
    return VideoOutputIssue{operation, native_code, std::move(message)};
}

std::int64_t current_native_error() noexcept {
    return static_cast<std::int64_t>(errno);
}

}  // namespace

struct H264FileOutputBackend::Impl {
    enum class State : std::uint8_t {
        Closed,
        Open,
        Flushed,
        Failed,
    };

    explicit Impl(std::filesystem::path output_path)
        : output_path_{std::move(output_path)} {}

    [[nodiscard]] contracts::output::VideoOutputOpenResult open();
    [[nodiscard]] contracts::output::VideoOutputConsumeResult consume(
        const model::EncodedVideoAccessUnit& access_unit);
    [[nodiscard]] contracts::output::VideoOutputFlushResult flush();
    void close() noexcept;

    std::filesystem::path output_path_;
    std::ofstream file_;
    State state_ = State::Closed;
};

contracts::output::VideoOutputOpenResult H264FileOutputBackend::Impl::open() {
    if (state_ != State::Closed) {
        return std::unexpected{issue(
            VideoOutputOperation::State, 0,
            "H.264 file output backend can only open from the closed state")};
    }
    if (output_path_.empty()) {
        return std::unexpected{issue(VideoOutputOperation::Open, 0,
                                     "H.264 output path must not be empty")};
    }

    errno = 0;
    file_.open(output_path_, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!file_.is_open()) {
        const auto native_code = current_native_error();
        file_.clear();
        return std::unexpected{issue(
            VideoOutputOperation::Open, native_code,
            "failed to open H.264 output file: " + output_path_.string())};
    }

    state_ = State::Open;
    return contracts::output::VideoOutputInfo{output_path_.string()};
}

contracts::output::VideoOutputConsumeResult
H264FileOutputBackend::Impl::consume(
    const model::EncodedVideoAccessUnit& access_unit) {
    if (state_ != State::Open) {
        return std::unexpected{issue(
            VideoOutputOperation::State, 0,
            "H.264 file output backend must be open before consuming")};
    }
    if (access_unit.annex_b.empty()) {
        state_ = State::Failed;
        return std::unexpected{issue(VideoOutputOperation::Consume, 0,
                                     "H.264 access unit must not be empty")};
    }
    if (access_unit.annex_b.size() >
        static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        state_ = State::Failed;
        return std::unexpected{issue(
            VideoOutputOperation::Consume, 0,
            "H.264 access unit exceeds the file stream size limit")};
    }

    errno = 0;
    file_.write(reinterpret_cast<const char*>(access_unit.annex_b.data()),
                static_cast<std::streamsize>(access_unit.annex_b.size()));
    if (!file_) {
        const auto native_code = current_native_error();
        state_ = State::Failed;
        return std::unexpected{issue(VideoOutputOperation::Consume, native_code,
                                     "failed to write H.264 access unit")};
    }

    return contracts::output::VideoOutputReceipt{
        1, static_cast<std::uint64_t>(access_unit.annex_b.size())};
}

contracts::output::VideoOutputFlushResult
H264FileOutputBackend::Impl::flush() {
    if (state_ != State::Open) {
        return std::unexpected{issue(
            VideoOutputOperation::State, 0,
            "H.264 file output backend can only flush an open session once")};
    }

    errno = 0;
    file_.flush();
    if (!file_) {
        const auto native_code = current_native_error();
        state_ = State::Failed;
        return std::unexpected{issue(VideoOutputOperation::Flush, native_code,
                                     "failed to flush H.264 output file")};
    }

    state_ = State::Flushed;
    return contracts::output::VideoOutputReceipt{};
}

void H264FileOutputBackend::Impl::close() noexcept {
    if (file_.is_open()) {
        file_.close();
    }
    file_.clear();
    state_ = State::Closed;
}

H264FileOutputBackend::H264FileOutputBackend(
    std::filesystem::path output_path)
    : impl_{std::make_unique<Impl>(std::move(output_path))} {}

H264FileOutputBackend::~H264FileOutputBackend() {
    impl_->close();
}

contracts::output::VideoOutputOpenResult H264FileOutputBackend::open() {
    return impl_->open();
}

contracts::output::VideoOutputConsumeResult H264FileOutputBackend::consume(
    const model::EncodedVideoAccessUnit& access_unit) {
    return impl_->consume(access_unit);
}

contracts::output::VideoOutputFlushResult H264FileOutputBackend::flush() {
    return impl_->flush();
}

void H264FileOutputBackend::close() noexcept {
    impl_->close();
}

}  // namespace semilive::publisher::infra::output
