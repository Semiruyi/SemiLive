#include <semilive/receiver/domain/rtp/rtp_session_filter.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace domain = semilive::receiver::domain;
namespace model = semilive::receiver::model;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

[[nodiscard]] model::RtpPacket packet(const std::uint8_t payload_type,
                                      const std::uint32_t ssrc,
                                      const std::uint16_t sequence = 0) {
    std::vector<std::byte> payload{
        std::byte{0x65},
        static_cast<std::byte>(sequence & 0xFFU),
    };
    return {std::move(payload), {}, false, payload_type, sequence, 90'000,
            ssrc, 0, 2};
}

const domain::RtpSessionAccepted& require_accepted(
    const domain::RtpSessionFilterResult& result,
    const std::string_view message) {
    const auto* accepted = std::get_if<domain::RtpSessionAccepted>(&result);
    require(accepted != nullptr, message);
    return *accepted;
}

void require_dropped(const domain::RtpSessionFilterResult& result,
                     const domain::RtpSessionDropReason reason,
                     const std::string_view message) {
    const auto* dropped = std::get_if<domain::RtpSessionDropped>(&result);
    require(dropped != nullptr, message);
    require(dropped->reason == reason,
            "session filter reported the wrong drop reason");
}

void validates_the_seven_bit_payload_type() {
    require(domain::validate_rtp_session_config({0, std::nullopt})
                .has_value(),
            "payload type 0 must be valid RTP configuration");
    require(domain::validate_rtp_session_config({127, std::nullopt})
                .has_value(),
            "payload type 127 must be valid RTP configuration");

    const auto invalid =
        domain::validate_rtp_session_config({128, std::nullopt});
    require(!invalid, "payload type above seven bits must be rejected");
    require(!invalid.error().empty(),
            "invalid session config must include a diagnostic message");
}

void learns_only_from_the_first_matching_payload_type() {
    domain::RtpSessionFilter filter{{96, std::nullopt}};

    const auto wrong_payload_type = filter.filter(packet(97, 0x11111111));
    require_dropped(wrong_payload_type,
                    domain::RtpSessionDropReason::PayloadTypeMismatch,
                    "foreign payload type must be dropped");
    require(!filter.stats().bound_ssrc,
            "foreign payload type must not bind the SSRC");

    const auto first = filter.filter(packet(96, 0x22222222, 7));
    const auto& first_accepted = require_accepted(
        first, "first matching packet must be accepted");
    require(first_accepted.ssrc_bound_now,
            "first matching packet must report SSRC binding");
    require(first_accepted.packet.ssrc() == 0x22222222,
            "accepted packet must preserve its SSRC");
    require(first_accepted.packet.sequence_number() == 7,
            "accepted packet must preserve its sequence number");
    require(first_accepted.packet.payload().size() == 2,
            "accepted packet must retain owned payload bytes");
    require(filter.stats().bound_ssrc == 0x22222222,
            "learned SSRC must be exposed in statistics");

    const auto second = filter.filter(packet(96, 0x22222222, 8));
    require(!require_accepted(second, "bound SSRC must remain accepted")
                 .ssrc_bound_now,
            "later packets must not report a new SSRC binding");

    const auto foreign_ssrc = filter.filter(packet(96, 0x33333333));
    require_dropped(foreign_ssrc,
                    domain::RtpSessionDropReason::SsrcMismatch,
                    "foreign SSRC must be dropped after binding");

    const auto& stats = filter.stats();
    require(stats.accepted_packets == 2,
            "accepted packet count must include matching packets only");
    require(stats.payload_type_mismatches == 1,
            "payload type mismatch count must be tracked");
    require(stats.ssrc_mismatches == 1,
            "SSRC mismatch count must be tracked");
}

void configured_ssrc_is_bound_before_the_first_packet() {
    domain::RtpSessionFilter filter{{96, 0x12345678}};
    require(filter.stats().bound_ssrc == 0x12345678,
            "configured SSRC must be bound immediately");

    const auto foreign = filter.filter(packet(96, 0x87654321));
    require_dropped(foreign, domain::RtpSessionDropReason::SsrcMismatch,
                    "packet outside configured SSRC must be dropped");

    const auto matching = filter.filter(packet(96, 0x12345678));
    require(!require_accepted(matching,
                              "configured SSRC packet must be accepted")
                 .ssrc_bound_now,
            "configured SSRC must not be reported as learned from a packet");
}

void reset_starts_a_fresh_learning_session() {
    domain::RtpSessionFilter filter{{96, std::nullopt}};
    require_accepted(filter.filter(packet(96, 1)),
                     "first session must learn an SSRC");
    require_dropped(filter.filter(packet(96, 2)),
                    domain::RtpSessionDropReason::SsrcMismatch,
                    "first session must reject a foreign SSRC");

    filter.reset();
    require(!filter.stats().bound_ssrc,
            "reset must clear a learned SSRC");
    require(filter.stats().accepted_packets == 0 &&
                filter.stats().ssrc_mismatches == 0,
            "reset must clear per-session counters");

    const auto next_session = filter.filter(packet(96, 2));
    require(require_accepted(next_session,
                             "new session must accept a new SSRC")
                .ssrc_bound_now,
            "new session must bind its first matching SSRC");
}

}  // namespace

int main() {
    try {
        validates_the_seven_bit_payload_type();
        learns_only_from_the_first_matching_payload_type();
        configured_ssrc_is_bound_before_the_first_packet();
        reset_starts_a_fresh_learning_session();
    } catch (const std::exception& error) {
        std::cerr << "RTP session filter test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
