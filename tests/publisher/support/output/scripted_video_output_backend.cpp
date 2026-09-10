#include "publisher/support/output/scripted_video_output_backend.hpp"

#include <string>
#include <utility>

namespace semilive::publisher::test_support {
namespace {

contracts::output::VideoOutputIssue missing_script_issue(
    const contracts::output::VideoOutputOperation operation,
    std::string message) {
    return {operation, 0, std::move(message)};
}

}  // namespace

void ScriptedVideoOutputBackend::queue_open_result(
    contracts::output::VideoOutputOpenResult result) {
    open_results_.push_back(std::move(result));
}

void ScriptedVideoOutputBackend::queue_consume_result(
    contracts::output::VideoOutputConsumeResult result) {
    consume_results_.push_back(std::move(result));
}

void ScriptedVideoOutputBackend::queue_flush_result(
    contracts::output::VideoOutputFlushResult result) {
    flush_results_.push_back(std::move(result));
}

const std::vector<ScriptedVideoOutputCall>&
ScriptedVideoOutputBackend::calls() const noexcept {
    return calls_;
}

const std::vector<model::EncodedVideoAccessUnit>&
ScriptedVideoOutputBackend::consumed_access_units() const noexcept {
    return consumed_access_units_;
}

contracts::output::VideoOutputOpenResult
ScriptedVideoOutputBackend::open() {
    calls_.push_back({ScriptedVideoOutputCallType::Open,
                      std::this_thread::get_id(), std::nullopt});
    if (open_results_.empty()) {
        return std::unexpected{missing_script_issue(
            contracts::output::VideoOutputOperation::Open,
            "no scripted video output open result")};
    }

    auto result = std::move(open_results_.front());
    open_results_.pop_front();
    return result;
}

contracts::output::VideoOutputConsumeResult
ScriptedVideoOutputBackend::consume(
    const model::EncodedVideoAccessUnit& access_unit) {
    calls_.push_back({ScriptedVideoOutputCallType::Consume,
                      std::this_thread::get_id(),
                      access_unit.source_sequence});
    consumed_access_units_.push_back(access_unit);
    if (consume_results_.empty()) {
        return std::unexpected{missing_script_issue(
            contracts::output::VideoOutputOperation::Consume,
            "no scripted video output consume result")};
    }

    auto result = std::move(consume_results_.front());
    consume_results_.pop_front();
    return result;
}

contracts::output::VideoOutputFlushResult
ScriptedVideoOutputBackend::flush() {
    calls_.push_back({ScriptedVideoOutputCallType::Flush,
                      std::this_thread::get_id(), std::nullopt});
    if (flush_results_.empty()) {
        return std::unexpected{missing_script_issue(
            contracts::output::VideoOutputOperation::Flush,
            "no scripted video output flush result")};
    }

    auto result = std::move(flush_results_.front());
    flush_results_.pop_front();
    return result;
}

void ScriptedVideoOutputBackend::close() noexcept {
    calls_.push_back({ScriptedVideoOutputCallType::Close,
                      std::this_thread::get_id(), std::nullopt});
}

}  // namespace semilive::publisher::test_support
