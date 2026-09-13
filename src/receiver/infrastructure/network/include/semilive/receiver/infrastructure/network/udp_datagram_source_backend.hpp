#pragma once

#include <semilive/receiver/contracts/network/datagram_source_backend.hpp>

#include <memory>

namespace semilive::receiver::infra::network {

class UdpDatagramSourceBackend final
    : public contracts::network::DatagramSourceBackend {
public:
    UdpDatagramSourceBackend();
    ~UdpDatagramSourceBackend() override;

    UdpDatagramSourceBackend(const UdpDatagramSourceBackend&) = delete;
    UdpDatagramSourceBackend& operator=(const UdpDatagramSourceBackend&) =
        delete;
    UdpDatagramSourceBackend(UdpDatagramSourceBackend&&) = delete;
    UdpDatagramSourceBackend& operator=(UdpDatagramSourceBackend&&) = delete;

    [[nodiscard]] contracts::network::DatagramSourceOpenResult open(
        const contracts::network::DatagramSourceConfig& config) override;
    [[nodiscard]] contracts::network::DatagramSourceReceiveResult receive_for(
        std::chrono::milliseconds timeout) override;
    void close() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::receiver::infra::network
