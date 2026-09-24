#pragma once

#include <semilive/receiver/domain/h264/h264_access_unit_assembler.hpp>
#include <semilive/receiver/domain/h264/h264_recovery_gate.hpp>
#include <semilive/receiver/domain/h264/h264_rtp_depacketizer.hpp>
#include <semilive/receiver/domain/rtp/rtp_reorder_buffer.hpp>
#include <semilive/receiver/domain/rtp/rtp_session_filter.hpp>
#include <semilive/receiver/domain/rtp/rtp_timestamp_mapper.hpp>
#include <semilive/receiver/model/network/udp_datagram.hpp>
#include <semilive/receiver/model/video/timed_h264_access_unit.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace semilive::receiver::domain {

class RtpReceptionStatistics;
class RtpMissingTracker;

struct H264RtpReceivePipelineConfig {
    RtpSessionConfig session;
    RtpReorderConfig reorder;
    H264RtpDepacketizerConfig depacketizer;
    H264AccessUnitAssemblerConfig assembler;
    RtpTimestampMapperConfig timestamp_mapper;
};

using H264RtpReceivePipelineConfigValidationResult =
    std::expected<void, std::string>;

[[nodiscard]] H264RtpReceivePipelineConfigValidationResult
validate_h264_rtp_receive_pipeline_config(
    const H264RtpReceivePipelineConfig& config);

using H264RtpReceivePipelineOutputs =
    std::vector<model::TimedH264AccessUnit>;

struct H264RtpReceivePipelineStats {
    std::uint64_t received_datagrams = 0;
    std::uint64_t parse_failures = 0;
    std::uint64_t session_drops = 0;
    std::uint64_t timestamp_mapping_failures = 0;
    std::uint64_t output_access_units = 0;
    RtpSessionFilterStats session_filter;
    RtpReorderStats reorder;
    H264RtpDepacketizerStats depacketizer;
    H264AccessUnitAssemblerStats assembler;
    H264RecoveryGateStats recovery;
    RtpTimestampMapperStats timestamp_mapper;
};

class H264RtpReceivePipeline final {
public:
    using Clock = model::UdpDatagram::Clock;

    explicit H264RtpReceivePipeline(
        H264RtpReceivePipelineConfig config = {},
        std::shared_ptr<RtpReceptionStatistics> reception_statistics = {},
        std::shared_ptr<RtpMissingTracker> missing_tracker = {});
    ~H264RtpReceivePipeline();

    H264RtpReceivePipeline(const H264RtpReceivePipeline&) = delete;
    H264RtpReceivePipeline& operator=(const H264RtpReceivePipeline&) =
        delete;
    H264RtpReceivePipeline(H264RtpReceivePipeline&&) noexcept;
    H264RtpReceivePipeline& operator=(
        H264RtpReceivePipeline&&) noexcept;

    [[nodiscard]] H264RtpReceivePipelineOutputs push(
        model::UdpDatagram datagram);
    [[nodiscard]] H264RtpReceivePipelineOutputs poll(Clock::time_point now);

    // Used by the worker when a complete AU is lost after leaving the
    // pipeline, such as output queue backpressure.
    void require_random_access(Clock::time_point observed_at) noexcept;

    [[nodiscard]] H264RtpReceivePipelineStats stats() const noexcept;
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::receiver::domain
