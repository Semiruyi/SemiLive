#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace semilive::publisher::contracts::output {

struct RtpRetransmissionSenderIssue {
    std::int64_t native_code = 0;
    std::string message;
};

using RtpRetransmissionSenderResult =
    std::expected<void, RtpRetransmissionSenderIssue>;

class RtpRetransmissionSender {
public:
    virtual ~RtpRetransmissionSender() = default;

    RtpRetransmissionSender(const RtpRetransmissionSender&) = delete;
    RtpRetransmissionSender& operator=(const RtpRetransmissionSender&) = delete;
    RtpRetransmissionSender(RtpRetransmissionSender&&) = delete;
    RtpRetransmissionSender& operator=(RtpRetransmissionSender&&) = delete;

    [[nodiscard]] virtual RtpRetransmissionSenderResult open() = 0;
    [[nodiscard]] virtual RtpRetransmissionSenderResult send(
        std::span<const std::byte> datagram) = 0;
    virtual void close() noexcept = 0;

protected:
    RtpRetransmissionSender() = default;
};

}  // namespace semilive::publisher::contracts::output
