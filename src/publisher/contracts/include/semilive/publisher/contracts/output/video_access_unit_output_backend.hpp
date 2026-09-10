#pragma once

#include <semilive/publisher/model/video/encoded_video_access_unit.hpp>

#include <cstdint>
#include <expected>
#include <string>

namespace semilive::publisher::contracts::output {

enum class VideoOutputOperation : std::uint8_t {
    State,
    Open,
    Consume,
    Flush,
    Close,
};

struct VideoOutputIssue {
    VideoOutputOperation operation = VideoOutputOperation::Open;
    std::int64_t native_code = 0;
    std::string message;
};

struct VideoOutputInfo {
    std::string output_name;
};

struct VideoOutputReceipt {
    std::uint64_t emitted_units = 0;
    std::uint64_t emitted_bytes = 0;
};

using VideoOutputOpenResult =
    std::expected<VideoOutputInfo, VideoOutputIssue>;
using VideoOutputConsumeResult =
    std::expected<VideoOutputReceipt, VideoOutputIssue>;
using VideoOutputFlushResult =
    std::expected<VideoOutputReceipt, VideoOutputIssue>;

class VideoAccessUnitOutputBackend {
public:
    virtual ~VideoAccessUnitOutputBackend() = default;

    VideoAccessUnitOutputBackend(const VideoAccessUnitOutputBackend&) = delete;
    VideoAccessUnitOutputBackend& operator=(
        const VideoAccessUnitOutputBackend&) = delete;
    VideoAccessUnitOutputBackend(VideoAccessUnitOutputBackend&&) = delete;
    VideoAccessUnitOutputBackend& operator=(
        VideoAccessUnitOutputBackend&&) = delete;

    [[nodiscard]] virtual VideoOutputOpenResult open() = 0;
    [[nodiscard]] virtual VideoOutputConsumeResult consume(
        const model::EncodedVideoAccessUnit& access_unit) = 0;
    [[nodiscard]] virtual VideoOutputFlushResult flush() = 0;
    virtual void close() noexcept = 0;

protected:
    VideoAccessUnitOutputBackend() = default;
};

}  // namespace semilive::publisher::contracts::output
