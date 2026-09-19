#include <semilive/receiver/domain/pipeline/h264_rtp_receive_pipeline.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace domain = semilive::receiver::domain;
namespace model = semilive::receiver::model;

using Clock = domain::H264RtpReceivePipeline::Clock;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
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

[[nodiscard]] model::UdpDatagram datagram(
    const std::uint16_t sequence,
    const std::uint32_t timestamp,
    const bool marker,
    const std::initializer_list<std::uint8_t> payload,
    const std::chrono::milliseconds received_at,
    const std::uint8_t payload_type = 96,
    const std::uint32_t ssrc = 0x1234'5678U) {
    std::vector<std::byte> bytes;
    bytes.reserve(12U + payload.size());
    bytes.push_back(std::byte{0x80});
    bytes.push_back(static_cast<std::byte>(
        static_cast<std::uint8_t>((marker ? 0x80U : 0U) | payload_type)));
    append_u16(bytes, sequence);
    append_u32(bytes, timestamp);
    append_u32(bytes, ssrc);
    for (const auto value : payload) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return {std::move(bytes), Clock::time_point{received_at}};
}

[[nodiscard]] std::vector<model::TimedH264AccessUnit> push_random_access(
    domain::H264RtpReceivePipeline& pipeline,
    const std::uint16_t first_sequence,
    const std::uint32_t timestamp,
    const std::chrono::milliseconds received_at = 0ms) {
    require(pipeline.push(datagram(first_sequence, timestamp, false,
                                   {0x67, 0x11}, received_at))
                .empty(),
            "SPS must wait for the AU marker");
    require(pipeline.push(datagram(
                static_cast<std::uint16_t>(first_sequence + 1U), timestamp,
                false, {0x68, 0x22}, received_at + 1ms))
                .empty(),
            "PPS must wait for the AU marker");
    return pipeline.push(datagram(
        static_cast<std::uint16_t>(first_sequence + 2U), timestamp, true,
        {0x65, 0x33}, received_at + 2ms));
}

void joins_all_stages_into_a_timed_playable_output() {
    domain::H264RtpReceivePipeline pipeline;

    require(pipeline.push(datagram(100, 1'000, true, {0x61, 0x01}, 0ms))
                .empty(),
            "startup inter frame must not escape the recovery gate");
    const auto recovered = push_random_access(pipeline, 101, 4'000, 1ms);
    require(recovered.size() == 1,
            "complete SPS/PPS/IDR AU must leave the full pipeline");
    require(recovered[0].presentation_time() == 0ns &&
                recovered[0].discontinuity_before(),
            "first output must establish time zero and decoder refresh");
    require(recovered[0].access_unit().nal_unit_count() == 3 &&
                recovered[0].access_unit().is_random_access_candidate(),
            "output must preserve the assembled random access AU");

    const auto next = pipeline.push(
        datagram(104, 7'000, true, {0x61, 0x44}, 5ms));
    require(next.size() == 1 &&
                next[0].presentation_time() == 33'333'333ns &&
                !next[0].discontinuity_before(),
            "streaming AU must receive media time without another reset");
}

void reorders_fu_a_fragments_before_assembling_the_au() {
    domain::H264RtpReceivePipeline pipeline;
    require(pipeline.push(datagram(10, 90'000, false, {0x67, 0x11}, 0ms))
                .empty(),
            "SPS must remain pending");
    require(pipeline.push(datagram(11, 90'000, false, {0x68, 0x22}, 1ms))
                .empty(),
            "PPS must remain pending");
    require(pipeline.push(
                datagram(13, 90'000, true, {0x7c, 0x45, 0xbb}, 2ms))
                .empty(),
            "future FU-A end must wait for its missing start");

    const auto output = pipeline.push(
        datagram(12, 90'000, false, {0x7c, 0x85, 0xaa}, 3ms));
    require(output.size() == 1,
            "gap fill must release and reconstruct the fragmented IDR");
    const std::vector<std::byte> expected{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x67}, std::byte{0x11},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x68}, std::byte{0x22},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x65}, std::byte{0xaa}, std::byte{0xbb}};
    require(std::ranges::equal(output[0].access_unit().annex_b(), expected),
            "pipeline must preserve NAL order and reconstruct FU-A bytes");
    require(pipeline.stats().reorder.reordered_packets == 1 &&
                pipeline.stats().depacketizer.fu_a_nal_units == 1,
            "nested stage statistics must remain observable");
}

void confirmed_loss_discards_the_gop_and_recovers_at_the_next_idr() {
    domain::H264RtpReceivePipeline pipeline;
    static_cast<void>(push_random_access(pipeline, 100, 1'000));

    require(pipeline.push(datagram(104, 4'000, true, {0x61}, 10ms))
                .empty(),
            "future packet must wait for the missing sequence");
    require(pipeline.poll(Clock::time_point{59ms}).empty(),
            "pending gap must not expire early");
    require(pipeline.poll(Clock::time_point{60ms}).empty(),
            "confirmed loss must discard the affected AU");
    require(pipeline.push(datagram(105, 7'000, true, {0x61}, 61ms))
                .empty(),
            "dependent frame after confirmed loss must be suppressed");

    const auto recovered = push_random_access(pipeline, 106, 10'000, 62ms);
    require(recovered.size() == 1 &&
                recovered[0].discontinuity_before(),
            "next complete random access AU must resume output atomically");
    const auto stats = pipeline.stats();
    require(stats.reorder.confirmed_lost_packets == 1 &&
                stats.depacketizer.sequence_gaps == 1 &&
                stats.recovery.recovery_points == 2 &&
                stats.recovery.recovery_episodes_started == 1 &&
                stats.recovery.recovery_episodes_completed == 1 &&
                stats.recovery.recovery_wait_total == 4ms,
            "loss must be visible through every affected pipeline stage");
}

void parse_and_session_rejections_do_not_contaminate_the_media_session() {
    domain::H264RtpReceivePipeline pipeline;
    require(pipeline.push({std::vector<std::byte>{std::byte{0x80}},
                           Clock::time_point{}})
                .empty(),
            "malformed RTP datagram must be rejected");
    require(pipeline.push(datagram(900, 1'000, true, {0x65}, 1ms, 97))
                .empty(),
            "foreign payload type must be ignored");

    const auto output = push_random_access(pipeline, 20, 2'000, 2ms);
    require(output.size() == 1,
            "rejected traffic must not bind SSRC or sequence state");
    const auto stats = pipeline.stats();
    require(stats.received_datagrams == 5 && stats.parse_failures == 1 &&
                stats.session_drops == 1 &&
                stats.session_filter.accepted_packets == 3,
            "pipeline boundary rejection statistics must be accurate");
}

void timestamp_failure_forces_random_access_recovery() {
    domain::H264RtpReceivePipeline pipeline;
    static_cast<void>(push_random_access(pipeline, 30, 90'000));

    require(pipeline.push(datagram(33, 90'000, true, {0x61}, 3ms))
                .empty(),
            "duplicate RTP timestamp must not reach output");
    require(pipeline.push(datagram(34, 93'000, true, {0x61}, 4ms))
                .empty(),
            "mapping failure must close the recovery gate");
    const auto recovered = push_random_access(pipeline, 35, 96'000, 5ms);
    require(recovered.size() == 1 &&
                recovered[0].discontinuity_before(),
            "timestamp fault must recover only at the next complete IDR");
    const auto stats = pipeline.stats();
    require(stats.timestamp_mapping_failures == 1 &&
                stats.recovery.external_discontinuities == 1,
            "timestamp failure must be counted and routed to recovery");
}

void external_output_drop_and_reset_have_session_scoped_behavior() {
    domain::H264RtpReceivePipeline pipeline;
    static_cast<void>(push_random_access(pipeline, 40, 1'000));
    pipeline.require_random_access(Clock::time_point{3ms});
    require(pipeline.push(datagram(43, 4'000, true, {0x61}, 3ms))
                .empty(),
            "external output loss must close the recovery gate");

    pipeline.reset();
    const auto stats = pipeline.stats();
    require(stats.received_datagrams == 0 &&
                stats.output_access_units == 0 &&
                !stats.session_filter.bound_ssrc,
            "reset must clear every stage and learned session identity");
    const auto fresh = push_random_access(
        pipeline, 500, 0xffff'ff00U, 10ms);
    require(fresh.size() == 1 && fresh[0].presentation_time() == 0ns,
            "reset must permit unrelated sequence and timestamp epochs");
}

void validates_every_nested_stage_configuration() {
    domain::H264RtpReceivePipelineConfig invalid_config;
    invalid_config.reorder.maximum_buffered_packets = 0;
    const auto validation =
        domain::validate_h264_rtp_receive_pipeline_config(invalid_config);
    require(!validation &&
                validation.error().find("RTP reorder buffer") !=
                    std::string::npos,
            "pipeline validation must identify the invalid nested stage");

    bool threw = false;
    try {
        domain::H264RtpReceivePipeline invalid_pipeline{invalid_config};
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "pipeline constructor must reject invalid configuration");
}

}  // namespace

int main() {
    try {
        joins_all_stages_into_a_timed_playable_output();
        reorders_fu_a_fragments_before_assembling_the_au();
        confirmed_loss_discards_the_gop_and_recovers_at_the_next_idr();
        parse_and_session_rejections_do_not_contaminate_the_media_session();
        timestamp_failure_forces_random_access_recovery();
        external_output_drop_and_reset_have_session_scoped_behavior();
        validates_every_nested_stage_configuration();
    } catch (const std::exception& error) {
        std::cerr << "H.264 RTP receive pipeline test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
