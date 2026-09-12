#include "h264_rtp_packetizer.hpp"

#include "annex_b_nal_splitter.hpp"

#include <algorithm>
#include <utility>

namespace semilive::publisher::infra::output::detail {
namespace {

constexpr std::size_t rtp_header_size = 12;
constexpr std::size_t fu_header_size = 2;
constexpr std::size_t minimum_datagram_size =
    rtp_header_size + fu_header_size + 1;
constexpr std::size_t maximum_udp_payload_size = 65'507;
constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000;
constexpr std::uint64_t h264_clock_rate = 90'000;

[[nodiscard]] std::uint8_t value(const std::byte byte) noexcept {
    return std::to_integer<std::uint8_t>(byte);
}

void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value >> 8U));
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::byte>(value >> 24U));
    bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
}

[[nodiscard]] std::uint32_t rtp_timestamp(
    const std::chrono::nanoseconds presentation_time,
    const std::uint32_t initial_timestamp) noexcept {
    const auto nanoseconds = static_cast<std::uint64_t>(
        presentation_time.count());
    const auto whole_seconds = nanoseconds / nanoseconds_per_second;
    const auto remainder = nanoseconds % nanoseconds_per_second;
    const auto whole_ticks =
        (whole_seconds % (std::uint64_t{1} << 32U)) * h264_clock_rate;
    const auto fractional_ticks =
        (remainder * h264_clock_rate + nanoseconds_per_second / 2U) /
        nanoseconds_per_second;
    return initial_timestamp +
           static_cast<std::uint32_t>(whole_ticks + fractional_ticks);
}

void write_rtp_header(std::vector<std::byte>& datagram,
                      const std::uint8_t payload_type,
                      const bool marker,
                      const std::uint16_t sequence,
                      const std::uint32_t timestamp,
                      const std::uint32_t ssrc) {
    datagram.clear();
    datagram.push_back(std::byte{0x80});
    datagram.push_back(static_cast<std::byte>(
        static_cast<std::uint8_t>((marker ? 0x80U : 0U) | payload_type)));
    append_u16(datagram, sequence);
    append_u32(datagram, timestamp);
    append_u32(datagram, ssrc);
}

[[nodiscard]] std::expected<void, std::string> validate_config(
    const H264RtpPacketizerConfig& config) {
    if (config.payload_type < 96 || config.payload_type > 127) {
        return std::unexpected{
            "RTP payload type must be in the dynamic range 96..127"};
    }
    if (config.max_datagram_bytes < minimum_datagram_size) {
        return std::unexpected{
            "RTP maximum datagram size is too small for an FU-A payload"};
    }
    if (config.max_datagram_bytes > maximum_udp_payload_size) {
        return std::unexpected{
            "RTP maximum datagram size exceeds the UDP payload limit"};
    }
    return {};
}

}  // namespace

H264RtpPacketizer::H264RtpPacketizer(H264RtpPacketizerConfig config)
    : config_{config} {
    datagram_.reserve(config.max_datagram_bytes);
}

RtpPacketizationResult H264RtpPacketizer::packetize(
    const std::span<const std::byte> annex_b,
    const std::chrono::nanoseconds presentation_time,
    RtpSessionState& session,
    const RtpDatagramEmitter& emit_datagram) {
    if (const auto valid_config = validate_config(config_); !valid_config) {
        return std::unexpected{valid_config.error()};
    }
    if (presentation_time < std::chrono::nanoseconds::zero()) {
        return std::unexpected{
            "RTP presentation time must not be negative"};
    }
    if (!emit_datagram) {
        return std::unexpected{"RTP datagram emitter must be callable"};
    }

    auto nal_units = split_annex_b_nal_units(annex_b);
    if (!nal_units) {
        return std::unexpected{std::move(nal_units.error())};
    }

    const auto timestamp = rtp_timestamp(presentation_time,
                                         session.initial_timestamp);
    const auto max_rtp_payload = config_.max_datagram_bytes - rtp_header_size;
    const auto max_fu_payload = max_rtp_payload - fu_header_size;
    RtpPacketizationReceipt receipt;

    const auto emit = [&](const bool marker,
                          const std::span<const std::byte> payload)
        -> std::expected<void, std::string> {
        const auto sequence = session.next_sequence++;
        write_rtp_header(datagram_, config_.payload_type, marker, sequence,
                         timestamp, session.ssrc);
        datagram_.insert(datagram_.end(), payload.begin(), payload.end());
        if (auto emitted = emit_datagram(datagram_); !emitted) {
            return std::unexpected{std::move(emitted.error())};
        }
        ++receipt.emitted_datagrams;
        receipt.emitted_bytes += datagram_.size();
        return {};
    };

    for (std::size_t nal_index = 0; nal_index < nal_units->size();
         ++nal_index) {
        const auto nal = (*nal_units)[nal_index];
        const bool last_nal = nal_index + 1 == nal_units->size();
        if (nal.size() <= max_rtp_payload) {
            if (auto emitted = emit(last_nal, nal); !emitted) {
                return std::unexpected{std::move(emitted.error())};
            }
            continue;
        }

        const auto nal_header = value(nal.front());
        const auto fu_indicator = static_cast<std::byte>(
            static_cast<std::uint8_t>((nal_header & 0xe0U) | 28U));
        const auto nal_type = static_cast<std::uint8_t>(nal_header & 0x1fU);
        const auto nal_payload = nal.subspan(1);
        for (std::size_t offset = 0; offset < nal_payload.size();) {
            const auto fragment_size =
                std::min(max_fu_payload, nal_payload.size() - offset);
            const bool first_fragment = offset == 0;
            const bool last_fragment =
                offset + fragment_size == nal_payload.size();
            const auto fu_header = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    (first_fragment ? 0x80U : 0U) |
                    (last_fragment ? 0x40U : 0U) | nal_type));

            write_rtp_header(datagram_, config_.payload_type,
                             last_nal && last_fragment,
                             session.next_sequence++, timestamp,
                             session.ssrc);
            datagram_.push_back(fu_indicator);
            datagram_.push_back(fu_header);
            const auto fragment = nal_payload.subspan(offset, fragment_size);
            datagram_.insert(datagram_.end(), fragment.begin(), fragment.end());
            if (auto emitted = emit_datagram(datagram_); !emitted) {
                return std::unexpected{std::move(emitted.error())};
            }
            ++receipt.emitted_datagrams;
            receipt.emitted_bytes += datagram_.size();
            offset += fragment_size;
        }
    }

    return receipt;
}

}  // namespace semilive::publisher::infra::output::detail
