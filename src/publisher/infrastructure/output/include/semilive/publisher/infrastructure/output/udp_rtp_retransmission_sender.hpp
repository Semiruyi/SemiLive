#pragma once

#include <semilive/publisher/contracts/output/rtp_retransmission_sender.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace semilive::publisher::infra::output {

struct UdpRtpRetransmissionSenderConfig {
    std::string destination_address;
    std::uint16_t destination_port = 0;
};

class UdpRtpRetransmissionSender final
    : public contracts::output::RtpRetransmissionSender {
public:
    explicit UdpRtpRetransmissionSender(
        UdpRtpRetransmissionSenderConfig config);
    ~UdpRtpRetransmissionSender() override;

    UdpRtpRetransmissionSender(const UdpRtpRetransmissionSender&) = delete;
    UdpRtpRetransmissionSender& operator=(
        const UdpRtpRetransmissionSender&) = delete;
    UdpRtpRetransmissionSender(UdpRtpRetransmissionSender&&) = delete;
    UdpRtpRetransmissionSender& operator=(
        UdpRtpRetransmissionSender&&) = delete;

    [[nodiscard]] contracts::output::RtpRetransmissionSenderResult open()
        override;
    [[nodiscard]] contracts::output::RtpRetransmissionSenderResult send(
        std::span<const std::byte> datagram) override;
    void close() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::publisher::infra::output
