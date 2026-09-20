#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace semilive::relay::infra::network {

enum class UdpRelaySocketOperation {
    State,
    Open,
    Receive,
    Send,
};

struct UdpRelaySocketIssue {
    UdpRelaySocketOperation operation = UdpRelaySocketOperation::State;
    std::int64_t native_code = 0;
    std::string message;
};

struct UdpRelaySocketConfig {
    std::string bind_address = "127.0.0.1";
    std::uint16_t bind_port = 0;
    std::string forward_address = "127.0.0.1";
    std::uint16_t forward_port = 0;
    std::size_t maximum_datagram_bytes = 65'507;
    std::size_t receive_buffer_bytes = 4U * 1024U * 1024U;
};

struct UdpRelaySocketInfo {
    std::string bound_address;
    std::uint16_t bound_port = 0;
    std::size_t maximum_datagram_bytes = 0;
    std::size_t receive_buffer_bytes = 0;
};

using UdpRelaySocketOpenResult =
    std::expected<UdpRelaySocketInfo, UdpRelaySocketIssue>;
using UdpRelaySocketReceiveResult =
    std::expected<std::optional<std::vector<std::byte>>,
                  UdpRelaySocketIssue>;
using UdpRelaySocketSendResult =
    std::expected<void, UdpRelaySocketIssue>;

class UdpRelaySocket final {
public:
    UdpRelaySocket();
    ~UdpRelaySocket();

    UdpRelaySocket(const UdpRelaySocket&) = delete;
    UdpRelaySocket& operator=(const UdpRelaySocket&) = delete;
    UdpRelaySocket(UdpRelaySocket&&) = delete;
    UdpRelaySocket& operator=(UdpRelaySocket&&) = delete;

    [[nodiscard]] UdpRelaySocketOpenResult open(
        const UdpRelaySocketConfig& config);
    [[nodiscard]] UdpRelaySocketReceiveResult receive_for(
        std::chrono::milliseconds timeout);
    [[nodiscard]] UdpRelaySocketSendResult send(
        std::span<const std::byte> datagram);
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::relay::infra::network
