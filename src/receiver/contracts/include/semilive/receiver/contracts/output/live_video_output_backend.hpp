#pragma once

#include <semilive/receiver/model/video/timed_h264_access_unit.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace semilive::receiver::contracts::output {

enum class LiveVideoOutputOperation : std::uint8_t {
    State,
    Open,
    Submit,
    Close,
};

struct LiveVideoOutputIssue {
    LiveVideoOutputOperation operation = LiveVideoOutputOperation::Open;
    std::int64_t native_code = 0;
    std::string message;
};

struct LiveVideoOutputInfo {
    std::string output_name;
};

enum class LiveVideoSubmitStatus : std::uint8_t {
    Accepted,
    DroppedBackpressure,
};

struct LiveVideoSubmitReceipt {
    LiveVideoSubmitStatus status = LiveVideoSubmitStatus::Accepted;
    std::size_t accepted_bytes = 0;
};

enum class LiveVideoOutputCloseMode : std::uint8_t {
    Normal,
    Abort,
};

using LiveVideoOutputOpenResult =
    std::expected<LiveVideoOutputInfo, LiveVideoOutputIssue>;
using LiveVideoOutputSubmitResult =
    std::expected<LiveVideoSubmitReceipt, LiveVideoOutputIssue>;

class LiveVideoOutputBackend {
public:
    virtual ~LiveVideoOutputBackend() = default;

    LiveVideoOutputBackend(const LiveVideoOutputBackend&) = delete;
    LiveVideoOutputBackend& operator=(const LiveVideoOutputBackend&) = delete;
    LiveVideoOutputBackend(LiveVideoOutputBackend&&) = delete;
    LiveVideoOutputBackend& operator=(LiveVideoOutputBackend&&) = delete;

    [[nodiscard]] virtual LiveVideoOutputOpenResult open() = 0;

    // Submit transfers ownership and must not wait for downstream capacity.
    // Temporary saturation is reported as DroppedBackpressure, not an error.
    [[nodiscard]] virtual LiveVideoOutputSubmitResult submit(
        model::TimedH264AccessUnit access_unit) = 0;

    virtual void close(LiveVideoOutputCloseMode mode) noexcept = 0;

protected:
    LiveVideoOutputBackend() = default;
};

}  // namespace semilive::receiver::contracts::output
