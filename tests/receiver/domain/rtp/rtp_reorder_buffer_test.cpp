#include <semilive/receiver/domain/rtp/rtp_reorder_buffer.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace domain = semilive::receiver::domain;
namespace model = semilive::receiver::model;

using Clock = domain::RtpReorderBuffer::Clock;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

[[nodiscard]] Clock::time_point at(const std::chrono::milliseconds time) {
    return Clock::time_point{time};
}

[[nodiscard]] model::RtpPacket packet(const std::uint16_t sequence,
                                      const std::chrono::milliseconds time) {
    std::vector<std::byte> payload{
        static_cast<std::byte>(sequence & 0xFFU)};
    return {std::move(payload), at(time), false, 96, sequence, 90'000,
            0x12345678, 0, 1};
}

[[nodiscard]] const domain::OrderedRtpPacket& ordered(
    const domain::RtpReorderEvent& event,
    const std::string_view message) {
    const auto* value = std::get_if<domain::OrderedRtpPacket>(&event);
    require(value != nullptr, message);
    return *value;
}

[[nodiscard]] const domain::RtpSequenceGap& gap(
    const domain::RtpReorderEvent& event,
    const std::string_view message) {
    const auto* value = std::get_if<domain::RtpSequenceGap>(&event);
    require(value != nullptr, message);
    return *value;
}

void emits_in_order_packets_immediately() {
    domain::RtpReorderBuffer buffer;

    const auto first = buffer.push(packet(100, 0ms));
    require(first.size() == 1,
            "first observed packet must be emitted immediately");
    require(ordered(first[0], "first event must contain a packet")
                .packet.sequence_number() == 100,
            "first emitted sequence must be preserved");

    const auto second = buffer.push(packet(101, 1ms));
    require(second.size() == 1,
            "next in-order packet must be emitted immediately");
    require(ordered(second[0], "second event must contain a packet")
                .packet.sequence_number() == 101,
            "second emitted sequence must be preserved");

    const auto stats = buffer.stats();
    require(stats.received_packets == 2 && stats.ordered_packets == 2,
            "in-order packet statistics must be tracked");
    require(stats.buffered_packets == 0 && stats.confirmed_gaps == 0,
            "in-order traffic must not create buffer state or gaps");
}

void buffers_future_packets_and_drains_when_the_gap_arrives() {
    domain::RtpReorderBuffer buffer;
    static_cast<void>(buffer.push(packet(100, 0ms)));

    require(buffer.push(packet(102, 10ms)).empty(),
            "future packet must wait in the reorder buffer");
    const auto released = buffer.push(packet(101, 20ms));
    require(released.size() == 2,
            "gap fill must emit the received and buffered packets");
    require(ordered(released[0], "gap fill must emit an ordered packet")
                .packet.sequence_number() == 101,
            "gap packet must be emitted first");
    require(ordered(released[1], "buffer drain must emit a packet")
                .packet.sequence_number() == 102,
            "buffered packet must follow the gap packet");

    const auto stats = buffer.stats();
    require(stats.reordered_packets == 1,
            "buffered packet release must count as reordering");
    require(stats.peak_buffered_packets == 1,
            "peak reorder occupancy must be tracked");
}

void timeout_confirms_loss_and_releases_the_nearest_packet() {
    domain::RtpReorderBuffer buffer;
    static_cast<void>(buffer.push(packet(100, 0ms)));
    static_cast<void>(buffer.push(packet(103, 10ms)));

    require(buffer.poll(at(59ms)).empty(),
            "gap must remain pending before its 50 ms deadline");
    const auto expired = buffer.poll(at(60ms));
    require(expired.size() == 2,
            "timeout must emit a gap followed by the nearest packet");

    const auto& loss = gap(expired[0], "timeout must emit a gap first");
    require(loss.first_missing == 101 && loss.next_received == 103 &&
                loss.missing_count == 2,
            "timeout gap must describe the missing sequence range");
    require(loss.cause == domain::RtpSequenceGapCause::HoldTimeout,
            "timeout gap must report its cause");
    require(ordered(expired[1], "timeout must release an ordered packet")
                .packet.sequence_number() == 103,
            "nearest buffered packet must resume ordered output");

    require(buffer.push(packet(102, 61ms)).empty(),
            "packet arriving after confirmed loss must be dropped as late");
    const auto stats = buffer.stats();
    require(stats.confirmed_gaps == 1 &&
                stats.confirmed_lost_packets == 2 &&
                stats.timeout_gaps == 1 && stats.late_packets == 1,
            "timeout and late packet statistics must be tracked");
}

void push_checks_the_deadline_before_accepting_a_late_gap_packet() {
    domain::RtpReorderBuffer buffer;
    static_cast<void>(buffer.push(packet(100, 0ms)));
    static_cast<void>(buffer.push(packet(102, 10ms)));

    const auto at_deadline = buffer.push(packet(101, 60ms));
    require(at_deadline.size() == 2,
            "push at the deadline must confirm loss before new input");
    require(gap(at_deadline[0], "deadline push must emit a gap")
                .first_missing == 101,
            "deadline gap must begin at the expected sequence");
    require(ordered(at_deadline[1],
                    "deadline push must release buffered data")
                .packet.sequence_number() == 102,
            "buffered future packet must be emitted after the gap");
    require(buffer.stats().late_packets == 1,
            "packet received at an expired deadline must count as late");
}

void draining_one_gap_preserves_the_next_gap_age() {
    domain::RtpReorderBuffer buffer;
    static_cast<void>(buffer.push(packet(100, 0ms)));
    static_cast<void>(buffer.push(packet(104, 10ms)));
    static_cast<void>(buffer.push(packet(102, 20ms)));

    const auto first_gap_filled = buffer.push(packet(101, 30ms));
    require(first_gap_filled.size() == 2,
            "filling the first gap must release sequences 101 and 102");
    require(buffer.poll(at(59ms)).empty(),
            "remaining gap must wait until the original evidence expires");

    const auto expired = buffer.poll(at(60ms));
    require(expired.size() == 2,
            "remaining gap must expire from the buffered packet arrival");
    require(gap(expired[0], "remaining gap must emit a loss event")
                    .first_missing == 103 &&
                ordered(expired[1],
                        "remaining gap must release sequence 104")
                        .packet.sequence_number() == 104,
            "new gap must not receive a fresh timeout after draining");
}

void one_poll_releases_every_already_expired_gap() {
    domain::RtpReorderBuffer buffer;
    static_cast<void>(buffer.push(packet(100, 0ms)));
    static_cast<void>(buffer.push(packet(102, 1ms)));
    static_cast<void>(buffer.push(packet(104, 2ms)));

    const auto expired = buffer.poll(at(52ms));
    require(expired.size() == 4,
            "one poll must emit both expired gaps and buffered packets");
    require(gap(expired[0], "first expired event must be a gap")
                    .first_missing == 101 &&
                ordered(expired[1], "first gap must release sequence 102")
                        .packet.sequence_number() == 102 &&
                gap(expired[2], "second expired event must be a gap")
                        .first_missing == 103 &&
                ordered(expired[3], "second gap must release sequence 104")
                        .packet.sequence_number() == 104,
            "expired gaps must be emitted in modular sequence order");
}

void capacity_confirms_loss_before_accepting_another_future_packet() {
    domain::RtpReorderBuffer buffer{{2, 1s}};
    static_cast<void>(buffer.push(packet(100, 0ms)));
    static_cast<void>(buffer.push(packet(102, 1ms)));
    static_cast<void>(buffer.push(packet(103, 2ms)));

    const auto forced = buffer.push(packet(104, 3ms));
    require(forced.size() == 4,
            "capacity pressure must emit a gap and all contiguous packets");
    const auto& loss = gap(forced[0], "capacity must emit a gap first");
    require(loss.first_missing == 101 && loss.next_received == 102 &&
                loss.missing_count == 1,
            "capacity gap must resume at the nearest buffered sequence");
    require(loss.cause == domain::RtpSequenceGapCause::BufferCapacity,
            "capacity gap must report its cause");
    require(ordered(forced[1], "capacity drain must emit sequence 102")
                    .packet.sequence_number() == 102 &&
                ordered(forced[2], "capacity drain must emit sequence 103")
                    .packet.sequence_number() == 103 &&
                ordered(forced[3], "incoming packet must emit sequence 104")
                    .packet.sequence_number() == 104,
            "capacity recovery must preserve sequence order");
    require(buffer.stats().capacity_gaps == 1,
            "capacity-forced gap must be counted");
}

void tracks_buffered_duplicates_and_emitted_late_packets() {
    domain::RtpReorderBuffer buffer;
    static_cast<void>(buffer.push(packet(100, 0ms)));
    static_cast<void>(buffer.push(packet(102, 1ms)));

    require(buffer.push(packet(102, 2ms)).empty(),
            "duplicate buffered packet must be dropped");
    require(buffer.push(packet(100, 3ms)).empty(),
            "already emitted packet must be dropped as late");
    const auto stats = buffer.stats();
    require(stats.duplicate_packets == 1 && stats.late_packets == 1,
            "duplicate and late drops must be counted separately");
    require(stats.buffered_packets == 1,
            "drops must not change the buffered future packet");
}

void handles_sequence_number_wraparound() {
    domain::RtpReorderBuffer buffer;
    static_cast<void>(buffer.push(packet(65'534, 0ms)));
    require(buffer.push(packet(0, 1ms)).empty(),
            "wrapped future packet must be buffered");

    const auto released = buffer.push(packet(65'535, 2ms));
    require(released.size() == 2,
            "last sequence before wrap must release sequence zero");
    require(ordered(released[0], "pre-wrap packet must be ordered")
                    .packet.sequence_number() == 65'535 &&
                ordered(released[1], "wrapped packet must be ordered")
                    .packet.sequence_number() == 0,
            "sequence wrap must preserve modular order");
}

void reset_discards_pending_state_and_statistics() {
    domain::RtpReorderBuffer buffer;
    static_cast<void>(buffer.push(packet(100, 0ms)));
    static_cast<void>(buffer.push(packet(102, 1ms)));
    buffer.reset();

    const auto stats = buffer.stats();
    require(stats.received_packets == 0 && stats.buffered_packets == 0,
            "reset must clear buffered data and per-session statistics");
    const auto fresh = buffer.push(packet(900, 2ms));
    require(fresh.size() == 1 &&
                ordered(fresh[0], "new session must emit its first packet")
                        .packet.sequence_number() == 900,
            "reset must allow a new sequence space");
}

void rejects_invalid_bounds() {
    require(!domain::validate_rtp_reorder_config({0, 50ms}),
            "zero packet capacity must be invalid");
    require(!domain::validate_rtp_reorder_config({32'768, 50ms}),
            "capacity spanning half the sequence space must be invalid");
    require(!domain::validate_rtp_reorder_config({64, 0ms}),
            "zero hold time must be invalid");

    bool threw = false;
    try {
        domain::RtpReorderBuffer invalid{{0, 50ms}};
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "constructor must reject invalid reorder bounds");
}

}  // namespace

int main() {
    try {
        emits_in_order_packets_immediately();
        buffers_future_packets_and_drains_when_the_gap_arrives();
        timeout_confirms_loss_and_releases_the_nearest_packet();
        push_checks_the_deadline_before_accepting_a_late_gap_packet();
        draining_one_gap_preserves_the_next_gap_age();
        one_poll_releases_every_already_expired_gap();
        capacity_confirms_loss_before_accepting_another_future_packet();
        tracks_buffered_duplicates_and_emitted_late_packets();
        handles_sequence_number_wraparound();
        reset_discards_pending_state_and_statistics();
        rejects_invalid_bounds();
    } catch (const std::exception& error) {
        std::cerr << "RTP reorder buffer test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
