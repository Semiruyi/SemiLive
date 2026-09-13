#include <semilive/receiver/domain/rtp/rtp_timestamp_mapper.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace semilive::receiver::domain {
namespace {

constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000;
constexpr std::uint32_t maximum_forward_delta =
    std::numeric_limits<std::int32_t>::max();

[[nodiscard]] std::optional<model::MediaTime> media_time_for(
    const std::uint64_t elapsed_ticks,
    const std::uint32_t clock_rate) noexcept {
    const auto whole_seconds = elapsed_ticks / clock_rate;
    const auto remaining_ticks = elapsed_ticks % clock_rate;
    const auto maximum_nanoseconds = static_cast<std::uint64_t>(
        std::numeric_limits<model::MediaTime::rep>::max());

    if (whole_seconds > maximum_nanoseconds / nanoseconds_per_second) {
        return std::nullopt;
    }
    const auto whole_nanoseconds = whole_seconds * nanoseconds_per_second;
    const auto fractional_nanoseconds =
        (remaining_ticks * nanoseconds_per_second) / clock_rate;
    if (fractional_nanoseconds > maximum_nanoseconds - whole_nanoseconds) {
        return std::nullopt;
    }

    return model::MediaTime{static_cast<model::MediaTime::rep>(
        whole_nanoseconds + fractional_nanoseconds)};
}

}  // namespace

RtpTimestampMapperConfigValidationResult
validate_rtp_timestamp_mapper_config(
    const RtpTimestampMapperConfig& config) {
    if (config.clock_rate == 0) {
        return std::unexpected{"RTP timestamp clock rate must be positive"};
    }
    return {};
}

RtpTimestampMapper::RtpTimestampMapper(RtpTimestampMapperConfig config)
    : config_{config} {
    const auto valid = validate_rtp_timestamp_mapper_config(config_);
    if (!valid) {
        throw std::invalid_argument{valid.error()};
    }
}

RtpTimestampMappingResult RtpTimestampMapper::map(
    model::PlayableH264AccessUnit playable) {
    ++stats_.input_access_units;
    const auto timestamp = playable.access_unit().rtp_timestamp();

    std::uint64_t next_extended_timestamp = timestamp;
    if (previous_timestamp_) {
        const auto forward_delta =
            static_cast<std::uint32_t>(timestamp - *previous_timestamp_);
        if (forward_delta == 0 || forward_delta > maximum_forward_delta) {
            ++stats_.non_increasing_timestamps;
            return std::unexpected{RtpTimestampMappingError{
                RtpTimestampMappingErrorCode::NonIncreasingTimestamp,
                *previous_timestamp_, timestamp}};
        }
        if (forward_delta >
            std::numeric_limits<std::uint64_t>::max() -
                extended_timestamp_) {
            ++stats_.media_time_overflows;
            return std::unexpected{RtpTimestampMappingError{
                RtpTimestampMappingErrorCode::MediaTimeOverflow,
                *previous_timestamp_, timestamp}};
        }
        next_extended_timestamp = extended_timestamp_ + forward_delta;
    }

    const auto initial_timestamp = previous_timestamp_
                                       ? initial_extended_timestamp_
                                       : next_extended_timestamp;
    const auto presentation_time = media_time_for(
        next_extended_timestamp - initial_timestamp, config_.clock_rate);
    if (!presentation_time) {
        ++stats_.media_time_overflows;
        return std::unexpected{RtpTimestampMappingError{
            RtpTimestampMappingErrorCode::MediaTimeOverflow,
            previous_timestamp_.value_or(timestamp), timestamp}};
    }

    if (previous_timestamp_ && timestamp < *previous_timestamp_) {
        ++stats_.timestamp_wraps;
    }
    if (!previous_timestamp_) {
        initial_extended_timestamp_ = next_extended_timestamp;
    }
    previous_timestamp_ = timestamp;
    extended_timestamp_ = next_extended_timestamp;
    ++stats_.mapped_access_units;
    stats_.last_extended_timestamp = extended_timestamp_;

    const auto discontinuity_before = playable.discontinuity_before();
    auto access_unit = std::move(playable).take_access_unit();
    return model::TimedH264AccessUnit{
        std::move(access_unit), *presentation_time,
        next_extended_timestamp, discontinuity_before};
}

RtpTimestampMapperStats RtpTimestampMapper::stats() const noexcept {
    return stats_;
}

void RtpTimestampMapper::reset() noexcept {
    previous_timestamp_.reset();
    initial_extended_timestamp_ = 0;
    extended_timestamp_ = 0;
    stats_ = {};
}

}  // namespace semilive::receiver::domain
