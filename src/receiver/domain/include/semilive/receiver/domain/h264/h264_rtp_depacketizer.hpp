#pragma once

#include <semilive/receiver/domain/rtp/rtp_reorder_buffer.hpp>
#include <semilive/receiver/model/video/h264_nal_unit.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace semilive::receiver::domain {

struct H264RtpDepacketizerConfig {
    std::size_t maximum_nal_unit_bytes = 8U * 1024U * 1024U;
};

using H264RtpDepacketizerConfigValidationResult =
    std::expected<void, std::string>;

[[nodiscard]] H264RtpDepacketizerConfigValidationResult
validate_h264_rtp_depacketizer_config(
    const H264RtpDepacketizerConfig& config);

enum class H264DepacketizationDiscontinuityCode : std::uint8_t {
    SequenceGap,
    EmptyPayload,
    ForbiddenBitSet,
    UnsupportedPacketizationMode,
    MalformedFuA,
    FragmentInterrupted,
    OrphanFuAFragment,
    FragmentSequenceMismatch,
    FragmentTimestampMismatch,
    FragmentHeaderMismatch,
    NalUnitTooLarge,
};

struct H264DepacketizationDiscontinuity {
    H264DepacketizationDiscontinuityCode code =
        H264DepacketizationDiscontinuityCode::EmptyPayload;
    std::uint16_t related_sequence = 0;
    std::uint16_t missing_packets = 0;
};

using H264DepacketizerEvent =
    std::variant<model::H264NalUnit, H264DepacketizationDiscontinuity>;
using H264DepacketizerEvents = std::vector<H264DepacketizerEvent>;

struct H264RtpDepacketizerStats {
    std::uint64_t input_packets = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t missing_rtp_packets = 0;
    std::uint64_t completed_nal_units = 0;
    std::uint64_t single_nal_units = 0;
    std::uint64_t fu_a_nal_units = 0;
    std::uint64_t discontinuities = 0;
    std::uint64_t dropped_packets = 0;
    std::uint64_t malformed_packets = 0;
    std::uint64_t unsupported_packets = 0;
    std::uint64_t orphan_fragments = 0;
    std::uint64_t abandoned_fragmented_nal_units = 0;
    std::uint64_t oversized_nal_units = 0;
    std::size_t pending_fragment_bytes = 0;
};

class H264RtpDepacketizer final {
public:
    explicit H264RtpDepacketizer(H264RtpDepacketizerConfig config = {});
    ~H264RtpDepacketizer();

    H264RtpDepacketizer(const H264RtpDepacketizer&) = delete;
    H264RtpDepacketizer& operator=(const H264RtpDepacketizer&) = delete;
    H264RtpDepacketizer(H264RtpDepacketizer&&) noexcept;
    H264RtpDepacketizer& operator=(H264RtpDepacketizer&&) noexcept;

    [[nodiscard]] H264DepacketizerEvents consume(RtpReorderEvent event);
    [[nodiscard]] H264RtpDepacketizerStats stats() const noexcept;
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::receiver::domain
