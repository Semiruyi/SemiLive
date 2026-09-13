#include <semilive/receiver/domain/h264/h264_access_unit_assembler.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace semilive::receiver::domain {
namespace {

constexpr std::array<std::byte, 4> annex_b_start_code{
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};

[[nodiscard]] bool is_vcl(const std::uint8_t nal_type) noexcept {
    return nal_type >= 1 && nal_type <= 5;
}

}  // namespace

H264AccessUnitAssemblerConfigValidationResult
validate_h264_access_unit_assembler_config(
    const H264AccessUnitAssemblerConfig& config) {
    if (config.maximum_access_unit_bytes < annex_b_start_code.size() + 1U) {
        return std::unexpected{
            "H.264 maximum access unit size must fit a start code and NAL"};
    }
    if (config.maximum_nal_units == 0) {
        return std::unexpected{
            "H.264 maximum NAL units per access unit must be positive"};
    }
    return {};
}

struct H264AccessUnitAssembler::Impl {
    struct PendingAccessUnit {
        std::vector<std::byte> annex_b;
        std::uint32_t rtp_timestamp = 0;
        std::uint16_t first_sequence = 0;
        std::uint16_t last_sequence = 0;
        std::size_t nal_unit_count = 0;
        bool contains_vcl = false;
        bool contains_idr = false;
        bool contains_sps = false;
        bool contains_pps = false;
    };

    explicit Impl(H264AccessUnitAssemblerConfig config) : config_{config} {}

    [[nodiscard]] H264AccessUnitAssemblerEvents consume(
        H264DepacketizerEvent event);
    [[nodiscard]] H264AccessUnitAssemblerStats stats() const noexcept;
    void reset() noexcept;

    void consume_nal(model::H264NalUnit nal,
                     H264AccessUnitAssemblerEvents& events);
    void consume_upstream_discontinuity(
        H264DepacketizationDiscontinuity discontinuity,
        H264AccessUnitAssemblerEvents& events);
    [[nodiscard]] bool discard_if_affected(const model::H264NalUnit& nal);
    void start_access_unit(const model::H264NalUnit& nal);
    [[nodiscard]] bool append_nal(const model::H264NalUnit& nal,
                                  H264AccessUnitAssemblerEvents& events);
    void complete_access_unit(const model::H264NalUnit& final_nal,
                              H264AccessUnitAssemblerEvents& events);
    void discard_pending() noexcept;
    void begin_discarding_pending_timestamp() noexcept;
    void emit_discontinuity(
        H264AccessUnitDiscontinuityCode code,
        std::optional<H264DepacketizationDiscontinuityCode> upstream_code,
        std::uint32_t timestamp,
        H264AccessUnitAssemblerEvents& events);

    H264AccessUnitAssemblerConfig config_;
    std::optional<PendingAccessUnit> pending_;
    bool discarding_access_unit_ = false;
    std::optional<std::uint32_t> discarded_timestamp_;
    H264AccessUnitAssemblerStats stats_;
};

H264AccessUnitAssemblerEvents H264AccessUnitAssembler::Impl::consume(
    H264DepacketizerEvent event) {
    H264AccessUnitAssemblerEvents events;
    if (auto* nal = std::get_if<model::H264NalUnit>(&event)) {
        consume_nal(std::move(*nal), events);
    } else {
        consume_upstream_discontinuity(
            std::get<H264DepacketizationDiscontinuity>(event), events);
    }
    return events;
}

H264AccessUnitAssemblerStats
H264AccessUnitAssembler::Impl::stats() const noexcept {
    auto snapshot = stats_;
    if (pending_) {
        snapshot.pending_bytes = pending_->annex_b.size();
        snapshot.pending_nal_units = pending_->nal_unit_count;
    }
    return snapshot;
}

void H264AccessUnitAssembler::Impl::reset() noexcept {
    pending_.reset();
    discarding_access_unit_ = false;
    discarded_timestamp_.reset();
    stats_ = {};
}

void H264AccessUnitAssembler::Impl::consume_nal(
    model::H264NalUnit nal,
    H264AccessUnitAssemblerEvents& events) {
    ++stats_.input_nal_units;
    if (nal.bytes().empty()) {
        begin_discarding_pending_timestamp();
        emit_discontinuity(H264AccessUnitDiscontinuityCode::EmptyNalUnit,
                           std::nullopt, nal.rtp_timestamp(), events);
        return;
    }

    if (discard_if_affected(nal)) {
        return;
    }

    if (pending_ && pending_->rtp_timestamp != nal.rtp_timestamp()) {
        const auto previous_timestamp = pending_->rtp_timestamp;
        discard_pending();
        ++stats_.timestamp_discontinuities;
        emit_discontinuity(
            H264AccessUnitDiscontinuityCode::TimestampChangedBeforeMarker,
            std::nullopt, previous_timestamp, events);
    }

    if (pending_) {
        const auto expected_sequence =
            static_cast<std::uint16_t>(pending_->last_sequence + 1U);
        if (nal.first_sequence() != expected_sequence) {
            const auto damaged_timestamp = pending_->rtp_timestamp;
            begin_discarding_pending_timestamp();
            ++stats_.sequence_discontinuities;
            emit_discontinuity(
                H264AccessUnitDiscontinuityCode::NalSequenceMismatch,
                std::nullopt, damaged_timestamp, events);
            static_cast<void>(discard_if_affected(nal));
            return;
        }
    } else {
        start_access_unit(nal);
    }

    if (!append_nal(nal, events)) {
        return;
    }
    if (nal.marker()) {
        complete_access_unit(nal, events);
    }
}

void H264AccessUnitAssembler::Impl::consume_upstream_discontinuity(
    const H264DepacketizationDiscontinuity discontinuity,
    H264AccessUnitAssemblerEvents& events) {
    ++stats_.upstream_discontinuities;
    if (pending_) {
        begin_discarding_pending_timestamp();
    } else if (!discarding_access_unit_) {
        discarding_access_unit_ = true;
        discarded_timestamp_.reset();
    }
    emit_discontinuity(H264AccessUnitDiscontinuityCode::UpstreamDiscontinuity,
                       discontinuity.code, 0, events);
}

bool H264AccessUnitAssembler::Impl::discard_if_affected(
    const model::H264NalUnit& nal) {
    if (!discarding_access_unit_) {
        return false;
    }

    if (!discarded_timestamp_) {
        discarded_timestamp_ = nal.rtp_timestamp();
        ++stats_.discarded_access_units;
    } else if (*discarded_timestamp_ != nal.rtp_timestamp()) {
        discarding_access_unit_ = false;
        discarded_timestamp_.reset();
        return false;
    }

    ++stats_.discarded_nal_units;
    if (nal.marker()) {
        discarding_access_unit_ = false;
        discarded_timestamp_.reset();
    }
    return true;
}

void H264AccessUnitAssembler::Impl::start_access_unit(
    const model::H264NalUnit& nal) {
    PendingAccessUnit next;
    next.annex_b.reserve(std::min(config_.maximum_access_unit_bytes,
                                  nal.bytes().size() +
                                      annex_b_start_code.size()));
    next.rtp_timestamp = nal.rtp_timestamp();
    next.first_sequence = nal.first_sequence();
    next.last_sequence = static_cast<std::uint16_t>(
        nal.first_sequence() - 1U);
    pending_ = std::move(next);
}

bool H264AccessUnitAssembler::Impl::append_nal(
    const model::H264NalUnit& nal,
    H264AccessUnitAssemblerEvents& events) {
    if (!pending_) {
        return false;
    }
    if (pending_->nal_unit_count >= config_.maximum_nal_units) {
        const auto timestamp = pending_->rtp_timestamp;
        ++stats_.excessive_nal_unit_counts;
        begin_discarding_pending_timestamp();
        emit_discontinuity(H264AccessUnitDiscontinuityCode::TooManyNalUnits,
                           std::nullopt, timestamp, events);
        static_cast<void>(discard_if_affected(nal));
        return false;
    }

    const auto nal_bytes = nal.bytes();
    const auto remaining = config_.maximum_access_unit_bytes -
                           pending_->annex_b.size();
    if (remaining < annex_b_start_code.size() ||
        nal_bytes.size() > remaining - annex_b_start_code.size()) {
        const auto timestamp = pending_->rtp_timestamp;
        ++stats_.oversized_access_units;
        begin_discarding_pending_timestamp();
        emit_discontinuity(H264AccessUnitDiscontinuityCode::AccessUnitTooLarge,
                           std::nullopt, timestamp, events);
        static_cast<void>(discard_if_affected(nal));
        return false;
    }

    pending_->annex_b.insert(pending_->annex_b.end(),
                             annex_b_start_code.begin(),
                             annex_b_start_code.end());
    pending_->annex_b.insert(pending_->annex_b.end(), nal_bytes.begin(),
                             nal_bytes.end());
    pending_->last_sequence = nal.last_sequence();
    ++pending_->nal_unit_count;
    const auto nal_type = nal.nal_unit_type();
    pending_->contains_vcl = pending_->contains_vcl || is_vcl(nal_type);
    pending_->contains_idr = pending_->contains_idr || nal_type == 5;
    pending_->contains_sps = pending_->contains_sps || nal_type == 7;
    pending_->contains_pps = pending_->contains_pps || nal_type == 8;
    stats_.peak_pending_bytes =
        std::max(stats_.peak_pending_bytes, pending_->annex_b.size());
    return true;
}

void H264AccessUnitAssembler::Impl::complete_access_unit(
    const model::H264NalUnit& final_nal,
    H264AccessUnitAssemblerEvents& events) {
    if (!pending_) {
        return;
    }

    auto completed = std::move(*pending_);
    pending_.reset();
    ++stats_.completed_access_units;
    events.emplace_back(model::H264AccessUnit{
        std::move(completed.annex_b), completed.rtp_timestamp,
        completed.first_sequence, completed.last_sequence,
        final_nal.completed_at(), completed.nal_unit_count,
        completed.contains_vcl, completed.contains_idr,
        completed.contains_sps, completed.contains_pps});
}

void H264AccessUnitAssembler::Impl::discard_pending() noexcept {
    if (pending_) {
        ++stats_.discarded_access_units;
        stats_.discarded_nal_units += pending_->nal_unit_count;
        pending_.reset();
    }
}

void H264AccessUnitAssembler::Impl::begin_discarding_pending_timestamp()
    noexcept {
    if (pending_) {
        discarding_access_unit_ = true;
        discarded_timestamp_ = pending_->rtp_timestamp;
        discard_pending();
    } else if (!discarding_access_unit_) {
        discarding_access_unit_ = true;
        discarded_timestamp_.reset();
    }
}

void H264AccessUnitAssembler::Impl::emit_discontinuity(
    const H264AccessUnitDiscontinuityCode code,
    std::optional<H264DepacketizationDiscontinuityCode> upstream_code,
    const std::uint32_t timestamp,
    H264AccessUnitAssemblerEvents& events) {
    events.emplace_back(H264AccessUnitDiscontinuity{
        code, std::move(upstream_code), timestamp});
}

H264AccessUnitAssembler::H264AccessUnitAssembler(
    H264AccessUnitAssemblerConfig config) {
    const auto valid = validate_h264_access_unit_assembler_config(config);
    if (!valid) {
        throw std::invalid_argument{valid.error()};
    }
    impl_ = std::make_unique<Impl>(config);
}

H264AccessUnitAssembler::~H264AccessUnitAssembler() = default;

H264AccessUnitAssembler::H264AccessUnitAssembler(
    H264AccessUnitAssembler&&) noexcept = default;

H264AccessUnitAssembler& H264AccessUnitAssembler::operator=(
    H264AccessUnitAssembler&&) noexcept = default;

H264AccessUnitAssemblerEvents H264AccessUnitAssembler::consume(
    H264DepacketizerEvent event) {
    return impl_->consume(std::move(event));
}

H264AccessUnitAssemblerStats H264AccessUnitAssembler::stats() const noexcept {
    return impl_->stats();
}

void H264AccessUnitAssembler::reset() noexcept {
    impl_->reset();
}

}  // namespace semilive::receiver::domain
