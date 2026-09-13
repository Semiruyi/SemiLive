#pragma once

#include <semilive/receiver/model/video/playable_h264_access_unit.hpp>
#include <semilive/receiver/model/video/timed_h264_access_unit.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace semilive::receiver::domain {

struct RtpTimestampMapperConfig {
    std::uint32_t clock_rate = 90'000;
};

using RtpTimestampMapperConfigValidationResult =
    std::expected<void, std::string>;

[[nodiscard]] RtpTimestampMapperConfigValidationResult
validate_rtp_timestamp_mapper_config(
    const RtpTimestampMapperConfig& config);

enum class RtpTimestampMappingErrorCode : std::uint8_t {
    NonIncreasingTimestamp,
    MediaTimeOverflow,
};

struct RtpTimestampMappingError {
    RtpTimestampMappingErrorCode code =
        RtpTimestampMappingErrorCode::NonIncreasingTimestamp;
    std::uint32_t previous_timestamp = 0;
    std::uint32_t current_timestamp = 0;
};

using RtpTimestampMappingResult =
    std::expected<model::TimedH264AccessUnit, RtpTimestampMappingError>;

struct RtpTimestampMapperStats {
    std::uint64_t input_access_units = 0;
    std::uint64_t mapped_access_units = 0;
    std::uint64_t timestamp_wraps = 0;
    std::uint64_t non_increasing_timestamps = 0;
    std::uint64_t media_time_overflows = 0;
    std::optional<std::uint64_t> last_extended_timestamp;
};

class RtpTimestampMapper final {
public:
    explicit RtpTimestampMapper(RtpTimestampMapperConfig config = {});

    [[nodiscard]] RtpTimestampMappingResult map(
        model::PlayableH264AccessUnit access_unit);

    [[nodiscard]] RtpTimestampMapperStats stats() const noexcept;
    void reset() noexcept;

private:
    RtpTimestampMapperConfig config_;
    std::optional<std::uint32_t> previous_timestamp_;
    std::uint64_t initial_extended_timestamp_ = 0;
    std::uint64_t extended_timestamp_ = 0;
    RtpTimestampMapperStats stats_;
};

}  // namespace semilive::receiver::domain
