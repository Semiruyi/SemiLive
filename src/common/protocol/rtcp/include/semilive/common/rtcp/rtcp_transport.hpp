#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace semilive::common::rtcp {

enum class TransportOperation : std::uint8_t {
    State,
    Open,
    Receive,
    Send,
};

struct TransportIssue {
    TransportOperation operation = TransportOperation::State;
    std::int64_t native_code = 0;
    std::string message;
};

struct TransportConfig {
    std::string bind_address = "0.0.0.0";
    std::uint16_t bind_port = 0;
    std::string peer_address;
    std::uint16_t peer_port = 0;
    std::size_t maximum_datagram_bytes = 1'500;
    std::size_t receive_buffer_bytes = 256U * 1024U;
};

struct TransportInfo {
    std::string bound_address;
    std::uint16_t bound_port = 0;
    std::size_t maximum_datagram_bytes = 0;
    std::size_t receive_buffer_bytes = 0;
};

struct ReceivedDatagram {
    std::vector<std::byte> bytes;
    std::chrono::steady_clock::time_point received_at;
};

using TransportOpenResult =
    std::expected<TransportInfo, TransportIssue>;
using TransportReceiveResult =
    std::expected<std::optional<ReceivedDatagram>, TransportIssue>;
using TransportSendResult = std::expected<void, TransportIssue>;

class Transport {
public:
    virtual ~Transport() = default;

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;

    [[nodiscard]] virtual TransportOpenResult open(
        const TransportConfig& config) = 0;
    [[nodiscard]] virtual TransportReceiveResult receive_for(
        std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual TransportSendResult send(
        std::span<const std::byte> datagram) = 0;
    virtual void close() noexcept = 0;

protected:
    Transport() = default;
};

}  // namespace semilive::common::rtcp
