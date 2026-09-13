#pragma once

#include <semilive/receiver/domain/h264/h264_rtp_depacketizer.hpp>
#include <semilive/receiver/model/video/h264_access_unit.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace semilive::receiver::domain {

struct H264AccessUnitAssemblerConfig {
    std::size_t maximum_access_unit_bytes = 16U * 1024U * 1024U;
    std::size_t maximum_nal_units = 256;
};

using H264AccessUnitAssemblerConfigValidationResult =
    std::expected<void, std::string>;

[[nodiscard]] H264AccessUnitAssemblerConfigValidationResult
validate_h264_access_unit_assembler_config(
    const H264AccessUnitAssemblerConfig& config);

enum class H264AccessUnitDiscontinuityCode : std::uint8_t {
    UpstreamDiscontinuity,
    TimestampChangedBeforeMarker,
    NalSequenceMismatch,
    AccessUnitTooLarge,
    TooManyNalUnits,
    EmptyNalUnit,
};

struct H264AccessUnitDiscontinuity {
    H264AccessUnitDiscontinuityCode code =
        H264AccessUnitDiscontinuityCode::UpstreamDiscontinuity;
    std::optional<H264DepacketizationDiscontinuityCode> upstream_code;
    std::uint32_t related_timestamp = 0;
};

using H264AccessUnitAssemblerEvent =
    std::variant<model::H264AccessUnit, H264AccessUnitDiscontinuity>;
using H264AccessUnitAssemblerEvents =
    std::vector<H264AccessUnitAssemblerEvent>;

struct H264AccessUnitAssemblerStats {
    std::uint64_t input_nal_units = 0;
    std::uint64_t upstream_discontinuities = 0;
    std::uint64_t completed_access_units = 0;
    std::uint64_t discarded_access_units = 0;
    std::uint64_t discarded_nal_units = 0;
    std::uint64_t timestamp_discontinuities = 0;
    std::uint64_t sequence_discontinuities = 0;
    std::uint64_t oversized_access_units = 0;
    std::uint64_t excessive_nal_unit_counts = 0;
    std::size_t pending_bytes = 0;
    std::size_t pending_nal_units = 0;
    std::size_t peak_pending_bytes = 0;
};

class H264AccessUnitAssembler final {
public:
    explicit H264AccessUnitAssembler(
        H264AccessUnitAssemblerConfig config = {});
    ~H264AccessUnitAssembler();

    H264AccessUnitAssembler(const H264AccessUnitAssembler&) = delete;
    H264AccessUnitAssembler& operator=(const H264AccessUnitAssembler&) =
        delete;
    H264AccessUnitAssembler(H264AccessUnitAssembler&&) noexcept;
    H264AccessUnitAssembler& operator=(
        H264AccessUnitAssembler&&) noexcept;

    [[nodiscard]] H264AccessUnitAssemblerEvents consume(
        H264DepacketizerEvent event);
    [[nodiscard]] H264AccessUnitAssemblerStats stats() const noexcept;
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::receiver::domain
