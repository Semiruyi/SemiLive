#include <semilive/receiver/domain/rtp/rtp_reorder_buffer.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace semilive::receiver::domain {
namespace {

constexpr std::uint16_t half_sequence_space = 0x8000U;

[[nodiscard]] std::uint16_t forward_distance(
    const std::uint16_t from,
    const std::uint16_t to) noexcept {
    return static_cast<std::uint16_t>(to - from);
}

}  // namespace

RtpReorderConfigValidationResult validate_rtp_reorder_config(
    const RtpReorderConfig& config) {
    if (config.maximum_buffered_packets == 0) {
        return std::unexpected{
            "RTP reorder buffer capacity must be positive"};
    }
    if (config.maximum_buffered_packets >= half_sequence_space) {
        return std::unexpected{
            "RTP reorder buffer capacity must be less than 32768 packets"};
    }
    if (config.maximum_hold_time <=
        std::chrono::milliseconds::zero()) {
        return std::unexpected{
            "RTP reorder maximum hold time must be positive"};
    }
    return {};
}

struct RtpReorderBuffer::Impl {
    struct BufferedPacket {
        model::RtpPacket packet;
        Clock::time_point buffered_at{};
    };

    using Buffer = std::map<std::uint16_t, BufferedPacket>;

    explicit Impl(RtpReorderConfig config) : config_{config} {}

    [[nodiscard]] RtpReorderEvents push(model::RtpPacket packet);
    [[nodiscard]] RtpReorderEvents poll(Clock::time_point now);
    [[nodiscard]] RtpReorderStats stats() const noexcept;
    void reset() noexcept;

    [[nodiscard]] Clock::time_point observe(Clock::time_point now) noexcept;
    void expire_gaps(Clock::time_point now, RtpReorderEvents& events);
    void confirm_gap(RtpSequenceGapCause cause, RtpReorderEvents& events);
    void emit_direct(model::RtpPacket packet, RtpReorderEvents& events);
    void drain_buffer(RtpReorderEvents& events);
    void refresh_gap_start() noexcept;
    [[nodiscard]] Buffer::iterator nearest_buffered_packet() noexcept;

    RtpReorderConfig config_;
    Buffer buffer_;
    std::optional<std::uint16_t> expected_sequence_;
    std::optional<Clock::time_point> observed_time_;
    std::optional<Clock::time_point> gap_started_at_;
    RtpReorderStats stats_;
};

RtpReorderEvents RtpReorderBuffer::Impl::push(model::RtpPacket packet) {
    RtpReorderEvents events;
    ++stats_.received_packets;

    const auto now = observe(packet.received_at());
    expire_gaps(now, events);

    if (!expected_sequence_) {
        expected_sequence_ =
            static_cast<std::uint16_t>(packet.sequence_number() + 1U);
        emit_direct(std::move(packet), events);
        return events;
    }

    while (true) {
        const auto sequence = packet.sequence_number();
        const auto distance = forward_distance(*expected_sequence_, sequence);
        if (distance == 0) {
            expected_sequence_ =
                static_cast<std::uint16_t>(*expected_sequence_ + 1U);
            emit_direct(std::move(packet), events);
            drain_buffer(events);
            return events;
        }

        if (distance >= half_sequence_space) {
            ++stats_.late_packets;
            return events;
        }

        if (buffer_.contains(sequence)) {
            ++stats_.duplicate_packets;
            return events;
        }

        if (buffer_.size() < config_.maximum_buffered_packets) {
            buffer_.emplace(
                sequence, BufferedPacket{std::move(packet), now});
            stats_.peak_buffered_packets =
                std::max(stats_.peak_buffered_packets, buffer_.size());
            refresh_gap_start();
            return events;
        }

        confirm_gap(RtpSequenceGapCause::BufferCapacity, events);
        expire_gaps(now, events);
    }
}

RtpReorderEvents RtpReorderBuffer::Impl::poll(
    const Clock::time_point now) {
    RtpReorderEvents events;
    expire_gaps(observe(now), events);
    return events;
}

RtpReorderStats RtpReorderBuffer::Impl::stats() const noexcept {
    auto snapshot = stats_;
    snapshot.buffered_packets = buffer_.size();
    return snapshot;
}

void RtpReorderBuffer::Impl::reset() noexcept {
    buffer_.clear();
    expected_sequence_.reset();
    observed_time_.reset();
    gap_started_at_.reset();
    stats_ = {};
}

RtpReorderBuffer::Clock::time_point RtpReorderBuffer::Impl::observe(
    const Clock::time_point now) noexcept {
    if (!observed_time_ || now > *observed_time_) {
        observed_time_ = now;
    }
    return *observed_time_;
}

void RtpReorderBuffer::Impl::expire_gaps(
    const Clock::time_point now,
    RtpReorderEvents& events) {
    while (gap_started_at_ &&
           now - *gap_started_at_ >= config_.maximum_hold_time) {
        confirm_gap(RtpSequenceGapCause::HoldTimeout, events);
    }
}

void RtpReorderBuffer::Impl::confirm_gap(
    const RtpSequenceGapCause cause,
    RtpReorderEvents& events) {
    if (!expected_sequence_ || buffer_.empty()) {
        gap_started_at_.reset();
        return;
    }

    const auto next = nearest_buffered_packet();
    if (next == buffer_.end()) {
        buffer_.clear();
        gap_started_at_.reset();
        return;
    }

    const auto next_sequence = next->first;
    const auto missing_count =
        forward_distance(*expected_sequence_, next_sequence);
    events.emplace_back(RtpSequenceGap{
        *expected_sequence_, next_sequence, missing_count, cause});
    ++stats_.confirmed_gaps;
    stats_.confirmed_lost_packets += missing_count;
    if (cause == RtpSequenceGapCause::HoldTimeout) {
        ++stats_.timeout_gaps;
    } else {
        ++stats_.capacity_gaps;
    }

    expected_sequence_ = next_sequence;
    drain_buffer(events);
}

void RtpReorderBuffer::Impl::emit_direct(
    model::RtpPacket packet,
    RtpReorderEvents& events) {
    ++stats_.ordered_packets;
    events.emplace_back(OrderedRtpPacket{std::move(packet)});
}

void RtpReorderBuffer::Impl::drain_buffer(RtpReorderEvents& events) {
    while (expected_sequence_) {
        const auto next = buffer_.find(*expected_sequence_);
        if (next == buffer_.end()) {
            break;
        }

        auto packet = std::move(next->second.packet);
        buffer_.erase(next);
        expected_sequence_ =
            static_cast<std::uint16_t>(*expected_sequence_ + 1U);
        ++stats_.ordered_packets;
        ++stats_.reordered_packets;
        events.emplace_back(OrderedRtpPacket{std::move(packet)});
    }
    refresh_gap_start();
}

void RtpReorderBuffer::Impl::refresh_gap_start() noexcept {
    if (buffer_.empty()) {
        gap_started_at_.reset();
        return;
    }

    auto earliest = Clock::time_point::max();
    for (const auto& [sequence, buffered] : buffer_) {
        static_cast<void>(sequence);
        earliest = std::min(earliest, buffered.buffered_at);
    }
    gap_started_at_ = earliest;
}

RtpReorderBuffer::Impl::Buffer::iterator
RtpReorderBuffer::Impl::nearest_buffered_packet() noexcept {
    if (!expected_sequence_) {
        return buffer_.end();
    }

    auto nearest = buffer_.end();
    auto nearest_distance = std::numeric_limits<std::uint16_t>::max();
    for (auto current = buffer_.begin(); current != buffer_.end(); ++current) {
        const auto distance =
            forward_distance(*expected_sequence_, current->first);
        if (distance != 0 && distance < half_sequence_space &&
            distance < nearest_distance) {
            nearest = current;
            nearest_distance = distance;
        }
    }
    return nearest;
}

RtpReorderBuffer::RtpReorderBuffer(RtpReorderConfig config) {
    const auto valid = validate_rtp_reorder_config(config);
    if (!valid) {
        throw std::invalid_argument{valid.error()};
    }
    impl_ = std::make_unique<Impl>(config);
}

RtpReorderBuffer::~RtpReorderBuffer() = default;

RtpReorderBuffer::RtpReorderBuffer(RtpReorderBuffer&&) noexcept = default;

RtpReorderBuffer& RtpReorderBuffer::operator=(
    RtpReorderBuffer&&) noexcept = default;

RtpReorderEvents RtpReorderBuffer::push(model::RtpPacket packet) {
    return impl_->push(std::move(packet));
}

RtpReorderEvents RtpReorderBuffer::poll(const Clock::time_point now) {
    return impl_->poll(now);
}

RtpReorderStats RtpReorderBuffer::stats() const noexcept {
    return impl_->stats();
}

void RtpReorderBuffer::reset() noexcept {
    impl_->reset();
}

}  // namespace semilive::receiver::domain
