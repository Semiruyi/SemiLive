#pragma once

#include <semilive/publisher/contracts/output/video_access_unit_output_backend.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <thread>
#include <vector>

namespace semilive::publisher::test_support {

enum class ScriptedVideoOutputCallType : std::uint8_t {
    Open,
    Consume,
    Flush,
    Close,
};

struct ScriptedVideoOutputCall {
    ScriptedVideoOutputCallType type = ScriptedVideoOutputCallType::Open;
    std::thread::id thread_id;
    std::optional<std::uint64_t> source_sequence;
};

class ScriptedVideoOutputBackend final
    : public contracts::output::VideoAccessUnitOutputBackend {
public:
    void queue_open_result(contracts::output::VideoOutputOpenResult result);
    void queue_consume_result(
        contracts::output::VideoOutputConsumeResult result);
    void queue_flush_result(contracts::output::VideoOutputFlushResult result);

    [[nodiscard]] const std::vector<ScriptedVideoOutputCall>& calls() const noexcept;
    [[nodiscard]] const std::vector<model::EncodedVideoAccessUnit>&
    consumed_access_units() const noexcept;

    [[nodiscard]] contracts::output::VideoOutputOpenResult open() override;
    [[nodiscard]] contracts::output::VideoOutputConsumeResult consume(
        const model::EncodedVideoAccessUnit& access_unit) override;
    [[nodiscard]] contracts::output::VideoOutputFlushResult flush() override;
    void close() noexcept override;

private:
    // The production contract is single-thread-owned. Tests inspect records only
    // after synchronizing with the worker, so locking would hide misuse.
    std::deque<contracts::output::VideoOutputOpenResult> open_results_;
    std::deque<contracts::output::VideoOutputConsumeResult> consume_results_;
    std::deque<contracts::output::VideoOutputFlushResult> flush_results_;
    std::vector<ScriptedVideoOutputCall> calls_;
    std::vector<model::EncodedVideoAccessUnit> consumed_access_units_;
};

}  // namespace semilive::publisher::test_support
