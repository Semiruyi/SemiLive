#pragma once

#include <semilive/common/rtcp/rtcp_transport.hpp>

#include <memory>

namespace semilive::common::rtcp {

class UdpTransport final : public Transport {
public:
    UdpTransport();
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;
    UdpTransport(UdpTransport&&) = delete;
    UdpTransport& operator=(UdpTransport&&) = delete;

    [[nodiscard]] TransportOpenResult open(
        const TransportConfig& config) override;
    [[nodiscard]] TransportReceiveResult receive_for(
        std::chrono::milliseconds timeout) override;
    [[nodiscard]] TransportSendResult send(
        std::span<const std::byte> datagram) override;
    void close() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::common::rtcp
