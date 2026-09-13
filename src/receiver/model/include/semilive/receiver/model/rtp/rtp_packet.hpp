#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace semilive::receiver::model {

class RtpPacket final {
public:
    using Clock = std::chrono::steady_clock;

    RtpPacket(std::vector<std::byte> storage,
              Clock::time_point received_at,
              bool marker,
              std::uint8_t payload_type,
              std::uint16_t sequence_number,
              std::uint32_t timestamp,
              std::uint32_t ssrc,
              std::size_t payload_offset,
              std::size_t payload_size) noexcept
        : storage_{std::move(storage)},
          received_at_{received_at},
          marker_{marker},
          payload_type_{payload_type},
          sequence_number_{sequence_number},
          timestamp_{timestamp},
          ssrc_{ssrc},
          payload_offset_{payload_offset},
          payload_size_{payload_size} {}

    [[nodiscard]] Clock::time_point received_at() const noexcept {
        return received_at_;
    }

    [[nodiscard]] bool marker() const noexcept {
        return marker_;
    }

    [[nodiscard]] std::uint8_t payload_type() const noexcept {
        return payload_type_;
    }

    [[nodiscard]] std::uint16_t sequence_number() const noexcept {
        return sequence_number_;
    }

    [[nodiscard]] std::uint32_t timestamp() const noexcept {
        return timestamp_;
    }

    [[nodiscard]] std::uint32_t ssrc() const noexcept {
        return ssrc_;
    }

    [[nodiscard]] std::span<const std::byte> payload() const noexcept {
        return {storage_.data() + payload_offset_, payload_size_};
    }

    [[nodiscard]] std::span<const std::byte> datagram() const noexcept {
        return storage_;
    }

private:
    std::vector<std::byte> storage_;
    Clock::time_point received_at_{};
    bool marker_ = false;
    std::uint8_t payload_type_ = 0;
    std::uint16_t sequence_number_ = 0;
    std::uint32_t timestamp_ = 0;
    std::uint32_t ssrc_ = 0;
    std::size_t payload_offset_ = 0;
    std::size_t payload_size_ = 0;
};

}  // namespace semilive::receiver::model
