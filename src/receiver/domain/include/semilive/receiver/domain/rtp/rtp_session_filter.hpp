#pragma once

#include <semilive/receiver/model/rtp/rtp_packet.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>

namespace semilive::receiver::domain {

struct RtpSessionConfig {
    std::uint8_t payload_type = 96;
    std::optional<std::uint32_t> ssrc;
};

using RtpSessionConfigValidationResult = std::expected<void, std::string>;

[[nodiscard]] RtpSessionConfigValidationResult
validate_rtp_session_config(const RtpSessionConfig& config);

enum class RtpSessionDropReason : std::uint8_t {
    PayloadTypeMismatch,
    SsrcMismatch,
};

struct RtpSessionAccepted {
    model::RtpPacket packet;
    bool ssrc_bound_now = false;
};

struct RtpSessionDropped {
    RtpSessionDropReason reason =
        RtpSessionDropReason::PayloadTypeMismatch;
};

using RtpSessionFilterResult =
    std::variant<RtpSessionAccepted, RtpSessionDropped>;

struct RtpSessionFilterStats {
    std::optional<std::uint32_t> bound_ssrc;
    std::uint64_t accepted_packets = 0;
    std::uint64_t payload_type_mismatches = 0;
    std::uint64_t ssrc_mismatches = 0;
};

class RtpSessionFilter final {
public:
    explicit RtpSessionFilter(RtpSessionConfig config) noexcept;

    [[nodiscard]] RtpSessionFilterResult filter(model::RtpPacket packet);
    [[nodiscard]] const RtpSessionFilterStats& stats() const noexcept;

    void reset() noexcept;

private:
    RtpSessionConfig config_;
    RtpSessionFilterStats stats_;
};

}  // namespace semilive::receiver::domain
