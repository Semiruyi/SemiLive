#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace semilive::receiver::domain {

struct PictureLossRequestConfig {
    bool enabled = true;
    std::chrono::milliseconds initial_wait{100};
    std::chrono::milliseconds retry_interval{500};
    std::chrono::milliseconds media_inactivity_timeout{1000};
};

// Single-thread-owned policy; no display-freeze assumptions or transport calls.
class PictureLossRequestPolicy final {
public:
    using Clock = std::chrono::steady_clock;

    explicit PictureLossRequestPolicy(PictureLossRequestConfig config = {})
        : config_{config} {}

    [[nodiscard]] bool observe(bool waiting,
                               std::optional<std::uint32_t> source,
                               std::uint64_t accepted_packets,
                               Clock::time_point now) noexcept {
        if (source != source_) {
            reset();
            source_ = source;
        }
        if (accepted_packets > accepted_packets_) {
            last_media_ = now;
        }
        accepted_packets_ = accepted_packets;
        if (!waiting || !source || !last_media_) {
            waiting_since_.reset();
            return false;
        }
        if (!waiting_since_) {
            waiting_since_ = now;
        }
        return config_.enabled && now - *waiting_since_ >= config_.initial_wait &&
               now - *last_media_ < config_.media_inactivity_timeout &&
               (!last_request_ || now - *last_request_ >= config_.retry_interval);
    }

    void requested(Clock::time_point now) noexcept { last_request_ = now; }

    void reset() noexcept {
        source_.reset();
        accepted_packets_ = 0;
        waiting_since_.reset();
        last_media_.reset();
        last_request_.reset();
    }

private:
    PictureLossRequestConfig config_;
    std::optional<std::uint32_t> source_;
    std::uint64_t accepted_packets_ = 0;
    std::optional<Clock::time_point> waiting_since_;
    std::optional<Clock::time_point> last_media_;
    std::optional<Clock::time_point> last_request_;
};

}  // namespace semilive::receiver::domain
