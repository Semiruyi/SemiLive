#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace semilive::publisher::contracts::output {

class RtpSenderObserver {
public:
    virtual ~RtpSenderObserver() = default;

    RtpSenderObserver(const RtpSenderObserver&) = delete;
    RtpSenderObserver& operator=(const RtpSenderObserver&) = delete;
    RtpSenderObserver(RtpSenderObserver&&) = delete;
    RtpSenderObserver& operator=(RtpSenderObserver&&) = delete;

    virtual void begin_session(std::uint32_t ssrc) noexcept = 0;
    virtual void record_sent_packet(
        std::uint32_t rtp_timestamp,
        std::size_t payload_octets,
        std::chrono::steady_clock::time_point sent_at) noexcept = 0;
    virtual void end_session() noexcept = 0;

protected:
    RtpSenderObserver() = default;
};

}  // namespace semilive::publisher::contracts::output
