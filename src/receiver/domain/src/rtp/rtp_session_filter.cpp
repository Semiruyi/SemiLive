#include <semilive/receiver/domain/rtp/rtp_session_filter.hpp>

#include <utility>

namespace semilive::receiver::domain {

RtpSessionConfigValidationResult validate_rtp_session_config(
    const RtpSessionConfig& config) {
    if (config.payload_type > 127) {
        return std::unexpected{
            "RTP session payload type must be in 0..127"};
    }
    return {};
}

RtpSessionFilter::RtpSessionFilter(RtpSessionConfig config) noexcept
    : config_{std::move(config)} {
    reset();
}

RtpSessionFilterResult RtpSessionFilter::filter(model::RtpPacket packet) {
    if (packet.payload_type() != config_.payload_type) {
        ++stats_.payload_type_mismatches;
        return RtpSessionDropped{
            RtpSessionDropReason::PayloadTypeMismatch};
    }

    if (stats_.bound_ssrc && packet.ssrc() != *stats_.bound_ssrc) {
        ++stats_.ssrc_mismatches;
        return RtpSessionDropped{RtpSessionDropReason::SsrcMismatch};
    }

    const bool ssrc_bound_now = !stats_.bound_ssrc.has_value();
    if (ssrc_bound_now) {
        stats_.bound_ssrc = packet.ssrc();
    }
    ++stats_.accepted_packets;
    return RtpSessionAccepted{std::move(packet), ssrc_bound_now};
}

const RtpSessionFilterStats& RtpSessionFilter::stats() const noexcept {
    return stats_;
}

void RtpSessionFilter::reset() noexcept {
    stats_ = {};
    stats_.bound_ssrc = config_.ssrc;
}

}  // namespace semilive::receiver::domain
