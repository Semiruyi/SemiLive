#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace semilive::publisher::infra::output::detail {

struct UdpSocketIssue {
    std::int64_t native_code = 0;
    std::string message;
};

using UdpSocketResult = std::expected<void, UdpSocketIssue>;

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&&) = delete;
    UdpSocket& operator=(UdpSocket&&) = delete;

    [[nodiscard]] UdpSocketResult open(
        std::string_view destination_address,
        std::uint16_t destination_port);
    [[nodiscard]] UdpSocketResult send(
        std::span<const std::byte> datagram);
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::publisher::infra::output::detail
