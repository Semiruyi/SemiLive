#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace semilive::receiver::model {

class H264AccessUnit final {
public:
    using Clock = std::chrono::steady_clock;

    H264AccessUnit(std::vector<std::byte> annex_b,
                   std::uint32_t rtp_timestamp,
                   std::uint16_t first_sequence,
                   std::uint16_t last_sequence,
                   Clock::time_point completed_at,
                   std::size_t nal_unit_count,
                   bool contains_vcl,
                   bool contains_idr,
                   bool contains_sps,
                   bool contains_pps) noexcept
        : annex_b_{std::move(annex_b)},
          rtp_timestamp_{rtp_timestamp},
          first_sequence_{first_sequence},
          last_sequence_{last_sequence},
          completed_at_{completed_at},
          nal_unit_count_{nal_unit_count},
          contains_vcl_{contains_vcl},
          contains_idr_{contains_idr},
          contains_sps_{contains_sps},
          contains_pps_{contains_pps} {}

    [[nodiscard]] std::span<const std::byte> annex_b() const noexcept {
        return annex_b_;
    }

    [[nodiscard]] std::uint32_t rtp_timestamp() const noexcept {
        return rtp_timestamp_;
    }

    [[nodiscard]] std::uint16_t first_sequence() const noexcept {
        return first_sequence_;
    }

    [[nodiscard]] std::uint16_t last_sequence() const noexcept {
        return last_sequence_;
    }

    [[nodiscard]] Clock::time_point completed_at() const noexcept {
        return completed_at_;
    }

    [[nodiscard]] std::size_t nal_unit_count() const noexcept {
        return nal_unit_count_;
    }

    [[nodiscard]] bool contains_vcl() const noexcept {
        return contains_vcl_;
    }

    [[nodiscard]] bool contains_idr() const noexcept {
        return contains_idr_;
    }

    [[nodiscard]] bool contains_sps() const noexcept {
        return contains_sps_;
    }

    [[nodiscard]] bool contains_pps() const noexcept {
        return contains_pps_;
    }

    [[nodiscard]] bool is_random_access_candidate() const noexcept {
        return contains_sps_ && contains_pps_ && contains_idr_;
    }

private:
    std::vector<std::byte> annex_b_;
    std::uint32_t rtp_timestamp_ = 0;
    std::uint16_t first_sequence_ = 0;
    std::uint16_t last_sequence_ = 0;
    Clock::time_point completed_at_{};
    std::size_t nal_unit_count_ = 0;
    bool contains_vcl_ = false;
    bool contains_idr_ = false;
    bool contains_sps_ = false;
    bool contains_pps_ = false;
};

}  // namespace semilive::receiver::model
