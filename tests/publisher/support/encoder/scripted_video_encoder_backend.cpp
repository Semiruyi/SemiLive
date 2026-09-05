#include "publisher/support/encoder/scripted_video_encoder_backend.hpp"

#include <string>
#include <utility>

namespace semilive::publisher::test_support {
namespace {

contracts::encoder::VideoEncoderIssue missing_script_issue(
    const contracts::encoder::VideoEncoderOperation operation,
    std::string message) {
    return {operation, 0, std::move(message)};
}

}  // namespace

void ScriptedVideoEncoderBackend::queue_open_result(
    contracts::encoder::VideoEncoderOpenResult result) {
    open_results_.push_back(std::move(result));
}

void ScriptedVideoEncoderBackend::queue_encode_result(
    contracts::encoder::VideoEncodeResult result) {
    encode_results_.push_back(std::move(result));
}

void ScriptedVideoEncoderBackend::queue_flush_result(
    contracts::encoder::VideoEncodeResult result) {
    flush_results_.push_back(std::move(result));
}

const std::vector<ScriptedVideoEncoderCall>&
ScriptedVideoEncoderBackend::calls() const noexcept {
    return calls_;
}

const std::vector<contracts::encoder::VideoEncoderConfig>&
ScriptedVideoEncoderBackend::open_configs() const noexcept {
    return open_configs_;
}

const std::vector<model::CapturedVideoFrame>&
ScriptedVideoEncoderBackend::encoded_frames() const noexcept {
    return encoded_frames_;
}

contracts::encoder::VideoEncoderOpenResult ScriptedVideoEncoderBackend::open(
    const contracts::encoder::VideoEncoderConfig& config) {
    calls_.push_back({ScriptedVideoEncoderCallType::Open,
                      std::this_thread::get_id(), std::nullopt});
    open_configs_.push_back(config);
    if (open_results_.empty()) {
        return std::unexpected{missing_script_issue(
            contracts::encoder::VideoEncoderOperation::Open,
            "no scripted video encoder open result")};
    }

    auto result = std::move(open_results_.front());
    open_results_.pop_front();
    return result;
}

contracts::encoder::VideoEncodeResult ScriptedVideoEncoderBackend::encode(
    const model::CapturedVideoFrame& frame) {
    calls_.push_back({ScriptedVideoEncoderCallType::Encode,
                      std::this_thread::get_id(), frame.sequence});
    encoded_frames_.push_back(frame);
    if (encode_results_.empty()) {
        return std::unexpected{missing_script_issue(
            contracts::encoder::VideoEncoderOperation::SendFrame,
            "no scripted video encoder encode result")};
    }

    auto result = std::move(encode_results_.front());
    encode_results_.pop_front();
    return result;
}

contracts::encoder::VideoEncodeResult ScriptedVideoEncoderBackend::flush() {
    calls_.push_back({ScriptedVideoEncoderCallType::Flush,
                      std::this_thread::get_id(), std::nullopt});
    if (flush_results_.empty()) {
        return std::unexpected{missing_script_issue(
            contracts::encoder::VideoEncoderOperation::Flush,
            "no scripted video encoder flush result")};
    }

    auto result = std::move(flush_results_.front());
    flush_results_.pop_front();
    return result;
}

void ScriptedVideoEncoderBackend::close() noexcept {
    calls_.push_back({ScriptedVideoEncoderCallType::Close,
                      std::this_thread::get_id(), std::nullopt});
}

}  // namespace semilive::publisher::test_support
