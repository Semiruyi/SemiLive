#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace semilive::receiver::model {

class H264NalUnit final {
public:
    using Clock = std::chrono::steady_clock;

    H264NalUnit(std::vector<std::byte> bytes,
                std::uint32_t rtp_timestamp,
                bool marker,
                std::uint16_t first_sequence,
                std::uint16_t last_sequence,
                Clock::time_point completed_at) noexcept
        : bytes_{std::move(bytes)},
          rtp_timestamp_{rtp_timestamp},
          marker_{marker},
          first_sequence_{first_sequence},
          last_sequence_{last_sequence},
          completed_at_{completed_at} {}

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::uint8_t nal_unit_type() const noexcept {
        if (bytes_.empty()) {
            return 0;
        }
        return static_cast<std::uint8_t>(
            std::to_integer<std::uint8_t>(bytes_.front()) & 0x1FU);
    }

    [[nodiscard]] std::uint32_t rtp_timestamp() const noexcept {
        return rtp_timestamp_;
    }

    [[nodiscard]] bool marker() const noexcept {
        return marker_;
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

private:
    std::vector<std::byte> bytes_;
    std::uint32_t rtp_timestamp_ = 0;
    bool marker_ = false;
    std::uint16_t first_sequence_ = 0;
    std::uint16_t last_sequence_ = 0;
    Clock::time_point completed_at_{};
};

}  // namespace semilive::receiver::model
