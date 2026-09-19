#include <semilive/receiver/domain/pipeline/h264_rtp_receive_pipeline.hpp>

#include <semilive/receiver/domain/rtp/rtp_parser.hpp>

#include <stdexcept>
#include <utility>
#include <variant>

namespace semilive::receiver::domain {
namespace {

[[nodiscard]] H264RtpReceivePipelineConfigValidationResult invalid(
    const char* component,
    const std::string& detail) {
    return std::unexpected{std::string{component} + ": " + detail};
}

}  // namespace

H264RtpReceivePipelineConfigValidationResult
validate_h264_rtp_receive_pipeline_config(
    const H264RtpReceivePipelineConfig& config) {
    if (const auto result = validate_rtp_session_config(config.session);
        !result) {
        return invalid("RTP session", result.error());
    }
    if (const auto result = validate_rtp_reorder_config(config.reorder);
        !result) {
        return invalid("RTP reorder buffer", result.error());
    }
    if (const auto result =
            validate_h264_rtp_depacketizer_config(config.depacketizer);
        !result) {
        return invalid("H.264 depacketizer", result.error());
    }
    if (const auto result =
            validate_h264_access_unit_assembler_config(config.assembler);
        !result) {
        return invalid("H.264 access unit assembler", result.error());
    }
    if (const auto result =
            validate_rtp_timestamp_mapper_config(config.timestamp_mapper);
        !result) {
        return invalid("RTP timestamp mapper", result.error());
    }
    return {};
}

struct H264RtpReceivePipeline::Impl {
    explicit Impl(const H264RtpReceivePipelineConfig& config)
        : session_filter_{config.session},
          reorder_{config.reorder},
          depacketizer_{config.depacketizer},
          assembler_{config.assembler},
          timestamp_mapper_{config.timestamp_mapper} {}

    [[nodiscard]] H264RtpReceivePipelineOutputs push(
        model::UdpDatagram datagram);
    [[nodiscard]] H264RtpReceivePipelineOutputs poll(Clock::time_point now);
    void require_random_access(Clock::time_point observed_at) noexcept;
    [[nodiscard]] H264RtpReceivePipelineStats stats() const noexcept;
    void reset() noexcept;

    [[nodiscard]] H264RtpReceivePipelineOutputs process(
        RtpReorderEvents events,
        Clock::time_point observed_at);
    void process_reorder_event(RtpReorderEvent event,
                               Clock::time_point observed_at,
                               H264RtpReceivePipelineOutputs& outputs);
    void process_depacketizer_event(
        H264DepacketizerEvent event,
        Clock::time_point observed_at,
        H264RtpReceivePipelineOutputs& outputs);
    void process_assembler_event(
        H264AccessUnitAssemblerEvent event,
        Clock::time_point observed_at,
        H264RtpReceivePipelineOutputs& outputs);

    RtpParser parser_;
    RtpSessionFilter session_filter_;
    RtpReorderBuffer reorder_;
    H264RtpDepacketizer depacketizer_;
    H264AccessUnitAssembler assembler_;
    H264RecoveryGate recovery_gate_;
    RtpTimestampMapper timestamp_mapper_;
    std::uint64_t received_datagrams_ = 0;
    std::uint64_t parse_failures_ = 0;
    std::uint64_t session_drops_ = 0;
    std::uint64_t timestamp_mapping_failures_ = 0;
    std::uint64_t output_access_units_ = 0;
};

H264RtpReceivePipelineOutputs H264RtpReceivePipeline::Impl::push(
    model::UdpDatagram datagram) {
    ++received_datagrams_;
    const auto observed_at = datagram.received_at;
    auto parsed = parser_.parse(std::move(datagram));
    if (!parsed) {
        ++parse_failures_;
        return {};
    }

    auto filtered = session_filter_.filter(std::move(*parsed));
    auto* accepted = std::get_if<RtpSessionAccepted>(&filtered);
    if (accepted == nullptr) {
        ++session_drops_;
        return {};
    }
    return process(reorder_.push(std::move(accepted->packet)), observed_at);
}

H264RtpReceivePipelineOutputs H264RtpReceivePipeline::Impl::poll(
    const Clock::time_point now) {
    return process(reorder_.poll(now), now);
}

void H264RtpReceivePipeline::Impl::require_random_access(
    const Clock::time_point observed_at) noexcept {
    recovery_gate_.require_random_access(observed_at);
}

H264RtpReceivePipelineStats
H264RtpReceivePipeline::Impl::stats() const noexcept {
    return {
        .received_datagrams = received_datagrams_,
        .parse_failures = parse_failures_,
        .session_drops = session_drops_,
        .timestamp_mapping_failures = timestamp_mapping_failures_,
        .output_access_units = output_access_units_,
        .session_filter = session_filter_.stats(),
        .reorder = reorder_.stats(),
        .depacketizer = depacketizer_.stats(),
        .assembler = assembler_.stats(),
        .recovery = recovery_gate_.stats(),
        .timestamp_mapper = timestamp_mapper_.stats(),
    };
}

void H264RtpReceivePipeline::Impl::reset() noexcept {
    session_filter_.reset();
    reorder_.reset();
    depacketizer_.reset();
    assembler_.reset();
    recovery_gate_.reset();
    timestamp_mapper_.reset();
    received_datagrams_ = 0;
    parse_failures_ = 0;
    session_drops_ = 0;
    timestamp_mapping_failures_ = 0;
    output_access_units_ = 0;
}

H264RtpReceivePipelineOutputs H264RtpReceivePipeline::Impl::process(
    RtpReorderEvents events,
    const Clock::time_point observed_at) {
    H264RtpReceivePipelineOutputs outputs;
    for (auto& event : events) {
        process_reorder_event(std::move(event), observed_at, outputs);
    }
    return outputs;
}

void H264RtpReceivePipeline::Impl::process_reorder_event(
    RtpReorderEvent event,
    const Clock::time_point observed_at,
    H264RtpReceivePipelineOutputs& outputs) {
    auto depacketizer_events = depacketizer_.consume(std::move(event));
    for (auto& depacketizer_event : depacketizer_events) {
        process_depacketizer_event(std::move(depacketizer_event), observed_at,
                                   outputs);
    }
}

void H264RtpReceivePipeline::Impl::process_depacketizer_event(
    H264DepacketizerEvent event,
    const Clock::time_point observed_at,
    H264RtpReceivePipelineOutputs& outputs) {
    auto assembler_events = assembler_.consume(std::move(event));
    for (auto& assembler_event : assembler_events) {
        process_assembler_event(std::move(assembler_event), observed_at,
                                outputs);
    }
}

void H264RtpReceivePipeline::Impl::process_assembler_event(
    H264AccessUnitAssemblerEvent event,
    const Clock::time_point observed_at,
    H264RtpReceivePipelineOutputs& outputs) {
    auto playable = recovery_gate_.consume(std::move(event), observed_at);
    if (!playable) {
        return;
    }

    auto mapped = timestamp_mapper_.map(std::move(*playable));
    if (!mapped) {
        ++timestamp_mapping_failures_;
        recovery_gate_.require_random_access(observed_at);
        return;
    }

    ++output_access_units_;
    outputs.push_back(std::move(*mapped));
}

H264RtpReceivePipeline::H264RtpReceivePipeline(
    H264RtpReceivePipelineConfig config) {
    const auto valid = validate_h264_rtp_receive_pipeline_config(config);
    if (!valid) {
        throw std::invalid_argument{valid.error()};
    }
    impl_ = std::make_unique<Impl>(config);
}

H264RtpReceivePipeline::~H264RtpReceivePipeline() = default;

H264RtpReceivePipeline::H264RtpReceivePipeline(
    H264RtpReceivePipeline&&) noexcept = default;

H264RtpReceivePipeline& H264RtpReceivePipeline::operator=(
    H264RtpReceivePipeline&&) noexcept = default;

H264RtpReceivePipelineOutputs H264RtpReceivePipeline::push(
    model::UdpDatagram datagram) {
    return impl_->push(std::move(datagram));
}

H264RtpReceivePipelineOutputs H264RtpReceivePipeline::poll(
    const Clock::time_point now) {
    return impl_->poll(now);
}

void H264RtpReceivePipeline::require_random_access(
    const Clock::time_point observed_at) noexcept {
    impl_->require_random_access(observed_at);
}

H264RtpReceivePipelineStats H264RtpReceivePipeline::stats() const noexcept {
    return impl_->stats();
}

void H264RtpReceivePipeline::reset() noexcept {
    impl_->reset();
}

}  // namespace semilive::receiver::domain
