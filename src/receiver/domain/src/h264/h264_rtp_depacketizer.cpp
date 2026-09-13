#include <semilive/receiver/domain/h264/h264_rtp_depacketizer.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace semilive::receiver::domain {
namespace {

constexpr std::uint8_t nal_type_mask = 0x1FU;
constexpr std::uint8_t nal_header_prefix_mask = 0xE0U;
constexpr std::uint8_t forbidden_bit_mask = 0x80U;
constexpr std::uint8_t fu_start_mask = 0x80U;
constexpr std::uint8_t fu_end_mask = 0x40U;
constexpr std::uint8_t fu_reserved_mask = 0x20U;
constexpr std::uint8_t fu_a_type = 28;

[[nodiscard]] std::uint8_t octet(const std::byte value) noexcept {
    return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] bool is_single_nal_type(const std::uint8_t type) noexcept {
    return type >= 1 && type <= 23;
}

}  // namespace

H264RtpDepacketizerConfigValidationResult
validate_h264_rtp_depacketizer_config(
    const H264RtpDepacketizerConfig& config) {
    if (config.maximum_nal_unit_bytes == 0) {
        return std::unexpected{
            "H.264 maximum NAL unit size must be positive"};
    }
    return {};
}

struct H264RtpDepacketizer::Impl {
    struct FragmentedNalUnit {
        std::vector<std::byte> bytes;
        std::uint8_t reconstructed_header = 0;
        std::uint32_t rtp_timestamp = 0;
        std::uint16_t first_sequence = 0;
        std::uint16_t last_sequence = 0;
    };

    explicit Impl(H264RtpDepacketizerConfig config) : config_{config} {}

    [[nodiscard]] H264DepacketizerEvents consume(RtpReorderEvent event);
    [[nodiscard]] H264RtpDepacketizerStats stats() const noexcept;
    void reset() noexcept;

    [[nodiscard]] H264DepacketizerEvents consume_packet(
        model::RtpPacket packet);
    [[nodiscard]] H264DepacketizerEvents consume_gap(RtpSequenceGap gap);
    void consume_fu_a(model::RtpPacket packet,
                      std::span<const std::byte> payload,
                      H264DepacketizerEvents& events);
    void emit_single_nal(model::RtpPacket packet,
                         std::span<const std::byte> payload,
                         H264DepacketizerEvents& events);
    void interrupt_fragment(std::uint16_t sequence,
                            H264DepacketizerEvents& events);
    void reject_packet(H264DepacketizationDiscontinuityCode code,
                       std::uint16_t sequence,
                       H264DepacketizerEvents& events);
    void emit_discontinuity(H264DepacketizationDiscontinuityCode code,
                            std::uint16_t sequence,
                            std::uint16_t missing_packets,
                            H264DepacketizerEvents& events);
    void abandon_fragment() noexcept;

    H264RtpDepacketizerConfig config_;
    std::optional<FragmentedNalUnit> fragment_;
    H264RtpDepacketizerStats stats_;
};

H264DepacketizerEvents H264RtpDepacketizer::Impl::consume(
    RtpReorderEvent event) {
    if (auto* packet = std::get_if<OrderedRtpPacket>(&event)) {
        return consume_packet(std::move(packet->packet));
    }
    return consume_gap(std::get<RtpSequenceGap>(event));
}

H264RtpDepacketizerStats H264RtpDepacketizer::Impl::stats() const noexcept {
    auto snapshot = stats_;
    snapshot.pending_fragment_bytes =
        fragment_ ? fragment_->bytes.size() : 0;
    return snapshot;
}

void H264RtpDepacketizer::Impl::reset() noexcept {
    fragment_.reset();
    stats_ = {};
}

H264DepacketizerEvents H264RtpDepacketizer::Impl::consume_packet(
    model::RtpPacket packet) {
    H264DepacketizerEvents events;
    ++stats_.input_packets;

    const auto payload = packet.payload();
    if (payload.empty()) {
        reject_packet(H264DepacketizationDiscontinuityCode::EmptyPayload,
                      packet.sequence_number(), events);
        return events;
    }

    const auto payload_header = octet(payload.front());
    if ((payload_header & forbidden_bit_mask) != 0) {
        reject_packet(H264DepacketizationDiscontinuityCode::ForbiddenBitSet,
                      packet.sequence_number(), events);
        return events;
    }

    const auto packetization_type =
        static_cast<std::uint8_t>(payload_header & nal_type_mask);
    if (is_single_nal_type(packetization_type)) {
        if (fragment_) {
            interrupt_fragment(packet.sequence_number(), events);
        }
        emit_single_nal(std::move(packet), payload, events);
        return events;
    }
    if (packetization_type == fu_a_type) {
        consume_fu_a(std::move(packet), payload, events);
        return events;
    }

    ++stats_.unsupported_packets;
    reject_packet(
        H264DepacketizationDiscontinuityCode::UnsupportedPacketizationMode,
        packet.sequence_number(), events);
    return events;
}

H264DepacketizerEvents H264RtpDepacketizer::Impl::consume_gap(
    const RtpSequenceGap gap) {
    H264DepacketizerEvents events;
    ++stats_.sequence_gaps;
    stats_.missing_rtp_packets += gap.missing_count;
    abandon_fragment();
    emit_discontinuity(H264DepacketizationDiscontinuityCode::SequenceGap,
                       gap.first_missing, gap.missing_count, events);
    return events;
}

void H264RtpDepacketizer::Impl::consume_fu_a(
    model::RtpPacket packet,
    const std::span<const std::byte> payload,
    H264DepacketizerEvents& events) {
    const auto sequence = packet.sequence_number();
    if (payload.size() < 3) {
        reject_packet(H264DepacketizationDiscontinuityCode::MalformedFuA,
                      sequence, events);
        return;
    }

    const auto indicator = octet(payload[0]);
    const auto fu_header = octet(payload[1]);
    const bool start = (fu_header & fu_start_mask) != 0;
    const bool end = (fu_header & fu_end_mask) != 0;
    const auto original_type =
        static_cast<std::uint8_t>(fu_header & nal_type_mask);
    if ((fu_header & fu_reserved_mask) != 0 || (start && end) ||
        !is_single_nal_type(original_type) || (packet.marker() && !end)) {
        reject_packet(H264DepacketizationDiscontinuityCode::MalformedFuA,
                      sequence, events);
        return;
    }

    const auto reconstructed_header = static_cast<std::uint8_t>(
        (indicator & nal_header_prefix_mask) | original_type);
    const auto fragment_payload = payload.subspan(2);
    if (start) {
        if (fragment_) {
            interrupt_fragment(sequence, events);
        }
        if (fragment_payload.size() + 1U >
            config_.maximum_nal_unit_bytes) {
            ++stats_.oversized_nal_units;
            reject_packet(
                H264DepacketizationDiscontinuityCode::NalUnitTooLarge,
                sequence, events);
            return;
        }

        FragmentedNalUnit next;
        next.bytes.reserve(std::min(config_.maximum_nal_unit_bytes,
                                    fragment_payload.size() + 1U));
        next.bytes.push_back(static_cast<std::byte>(reconstructed_header));
        next.bytes.insert(next.bytes.end(), fragment_payload.begin(),
                          fragment_payload.end());
        next.reconstructed_header = reconstructed_header;
        next.rtp_timestamp = packet.timestamp();
        next.first_sequence = sequence;
        next.last_sequence = sequence;
        fragment_ = std::move(next);
        return;
    }

    if (!fragment_) {
        ++stats_.orphan_fragments;
        reject_packet(
            H264DepacketizationDiscontinuityCode::OrphanFuAFragment,
            sequence, events);
        return;
    }

    const auto expected_sequence =
        static_cast<std::uint16_t>(fragment_->last_sequence + 1U);
    if (sequence != expected_sequence) {
        reject_packet(
            H264DepacketizationDiscontinuityCode::FragmentSequenceMismatch,
            sequence, events);
        return;
    }
    if (packet.timestamp() != fragment_->rtp_timestamp) {
        reject_packet(
            H264DepacketizationDiscontinuityCode::FragmentTimestampMismatch,
            sequence, events);
        return;
    }
    if (reconstructed_header != fragment_->reconstructed_header) {
        reject_packet(
            H264DepacketizationDiscontinuityCode::FragmentHeaderMismatch,
            sequence, events);
        return;
    }
    if (fragment_payload.size() >
        config_.maximum_nal_unit_bytes - fragment_->bytes.size()) {
        ++stats_.oversized_nal_units;
        reject_packet(H264DepacketizationDiscontinuityCode::NalUnitTooLarge,
                      sequence, events);
        return;
    }

    fragment_->bytes.insert(fragment_->bytes.end(), fragment_payload.begin(),
                            fragment_payload.end());
    fragment_->last_sequence = sequence;
    if (!end) {
        return;
    }

    auto completed = std::move(*fragment_);
    fragment_.reset();
    ++stats_.completed_nal_units;
    ++stats_.fu_a_nal_units;
    events.emplace_back(model::H264NalUnit{
        std::move(completed.bytes), completed.rtp_timestamp, packet.marker(),
        completed.first_sequence, sequence, packet.received_at()});
}

void H264RtpDepacketizer::Impl::emit_single_nal(
    model::RtpPacket packet,
    const std::span<const std::byte> payload,
    H264DepacketizerEvents& events) {
    if (payload.size() > config_.maximum_nal_unit_bytes) {
        ++stats_.oversized_nal_units;
        reject_packet(H264DepacketizationDiscontinuityCode::NalUnitTooLarge,
                      packet.sequence_number(), events);
        return;
    }

    std::vector<std::byte> bytes{payload.begin(), payload.end()};
    ++stats_.completed_nal_units;
    ++stats_.single_nal_units;
    events.emplace_back(model::H264NalUnit{
        std::move(bytes), packet.timestamp(), packet.marker(),
        packet.sequence_number(), packet.sequence_number(),
        packet.received_at()});
}

void H264RtpDepacketizer::Impl::interrupt_fragment(
    const std::uint16_t sequence,
    H264DepacketizerEvents& events) {
    abandon_fragment();
    emit_discontinuity(
        H264DepacketizationDiscontinuityCode::FragmentInterrupted,
        sequence, 0, events);
}

void H264RtpDepacketizer::Impl::reject_packet(
    const H264DepacketizationDiscontinuityCode code,
    const std::uint16_t sequence,
    H264DepacketizerEvents& events) {
    ++stats_.dropped_packets;
    if (code !=
            H264DepacketizationDiscontinuityCode::UnsupportedPacketizationMode &&
        code != H264DepacketizationDiscontinuityCode::NalUnitTooLarge) {
        ++stats_.malformed_packets;
    }
    abandon_fragment();
    emit_discontinuity(code, sequence, 0, events);
}

void H264RtpDepacketizer::Impl::emit_discontinuity(
    const H264DepacketizationDiscontinuityCode code,
    const std::uint16_t sequence,
    const std::uint16_t missing_packets,
    H264DepacketizerEvents& events) {
    ++stats_.discontinuities;
    events.emplace_back(
        H264DepacketizationDiscontinuity{code, sequence, missing_packets});
}

void H264RtpDepacketizer::Impl::abandon_fragment() noexcept {
    if (fragment_) {
        ++stats_.abandoned_fragmented_nal_units;
        fragment_.reset();
    }
}

H264RtpDepacketizer::H264RtpDepacketizer(
    H264RtpDepacketizerConfig config) {
    const auto valid = validate_h264_rtp_depacketizer_config(config);
    if (!valid) {
        throw std::invalid_argument{valid.error()};
    }
    impl_ = std::make_unique<Impl>(config);
}

H264RtpDepacketizer::~H264RtpDepacketizer() = default;

H264RtpDepacketizer::H264RtpDepacketizer(
    H264RtpDepacketizer&&) noexcept = default;

H264RtpDepacketizer& H264RtpDepacketizer::operator=(
    H264RtpDepacketizer&&) noexcept = default;

H264DepacketizerEvents H264RtpDepacketizer::consume(
    RtpReorderEvent event) {
    return impl_->consume(std::move(event));
}

H264RtpDepacketizerStats H264RtpDepacketizer::stats() const noexcept {
    return impl_->stats();
}

void H264RtpDepacketizer::reset() noexcept {
    impl_->reset();
}

}  // namespace semilive::receiver::domain
