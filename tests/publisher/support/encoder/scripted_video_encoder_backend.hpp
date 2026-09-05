#pragma once

#include "publisher/contracts/encoder/video_encoder_backend.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <thread>
#include <vector>

namespace semilive::publisher::test_support {

enum class ScriptedVideoEncoderCallType : std::uint8_t {
    Open,
    Encode,
    Flush,
    Close,
};

struct ScriptedVideoEncoderCall {
    ScriptedVideoEncoderCallType type = ScriptedVideoEncoderCallType::Open;
    std::thread::id thread_id;
    std::optional<std::uint64_t> source_sequence;
};

class ScriptedVideoEncoderBackend final
    : public contracts::encoder::VideoEncoderBackend {
public:
    void queue_open_result(contracts::encoder::VideoEncoderOpenResult result);
    void queue_encode_result(contracts::encoder::VideoEncodeResult result);
    void queue_flush_result(contracts::encoder::VideoEncodeResult result);

    [[nodiscard]] const std::vector<ScriptedVideoEncoderCall>& calls() const noexcept;
    [[nodiscard]] const std::vector<contracts::encoder::VideoEncoderConfig>&
    open_configs() const noexcept;
    [[nodiscard]] const std::vector<model::CapturedVideoFrame>&
    encoded_frames() const noexcept;

    [[nodiscard]] contracts::encoder::VideoEncoderOpenResult open(
        const contracts::encoder::VideoEncoderConfig& config) override;
    [[nodiscard]] contracts::encoder::VideoEncodeResult encode(
        const model::CapturedVideoFrame& frame) override;
    [[nodiscard]] contracts::encoder::VideoEncodeResult flush() override;
    void close() noexcept override;

private:
    // The real contract is single-thread-owned. Tests inspect these records only
    // after synchronizing with the worker, so locking here would hide misuse.
    std::deque<contracts::encoder::VideoEncoderOpenResult> open_results_;
    std::deque<contracts::encoder::VideoEncodeResult> encode_results_;
    std::deque<contracts::encoder::VideoEncodeResult> flush_results_;
    std::vector<ScriptedVideoEncoderCall> calls_;
    std::vector<contracts::encoder::VideoEncoderConfig> open_configs_;
    std::vector<model::CapturedVideoFrame> encoded_frames_;
};

}  // namespace semilive::publisher::test_support
