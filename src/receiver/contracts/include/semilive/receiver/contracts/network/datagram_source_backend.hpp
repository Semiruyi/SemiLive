#pragma once

#include <semilive/receiver/model/network/udp_datagram.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <variant>

namespace semilive::receiver::contracts::network {

struct DatagramSourceConfig {
    std::string bind_address = "0.0.0.0";
    std::uint16_t bind_port = 5004;
    std::size_t maximum_datagram_bytes = 65'507;
};

enum class DatagramSourceOperation : std::uint8_t {
    State,
    Open,
    Receive,
    Close,
};

struct DatagramSourceIssue {
    DatagramSourceOperation operation = DatagramSourceOperation::Open;
    std::int64_t native_code = 0;
    std::string message;
};

struct DatagramSourceInfo {
    std::string bound_address;
    std::uint16_t bound_port = 0;
    std::size_t maximum_datagram_bytes = 0;
};

struct DatagramReceiveTimeout {};

using DatagramSourceObservation =
    std::variant<model::UdpDatagram, DatagramReceiveTimeout>;
using DatagramSourceOpenResult =
    std::expected<DatagramSourceInfo, DatagramSourceIssue>;
using DatagramSourceReceiveResult =
    std::expected<DatagramSourceObservation, DatagramSourceIssue>;

class DatagramSourceBackend {
public:
    virtual ~DatagramSourceBackend() = default;

    DatagramSourceBackend(const DatagramSourceBackend&) = delete;
    DatagramSourceBackend& operator=(const DatagramSourceBackend&) = delete;
    DatagramSourceBackend(DatagramSourceBackend&&) = delete;
    DatagramSourceBackend& operator=(DatagramSourceBackend&&) = delete;

    [[nodiscard]] virtual DatagramSourceOpenResult open(
        const DatagramSourceConfig& config) = 0;

    // Implementations must bound the wait by timeout so the owning worker can
    // observe stop requests and poll protocol timers.
    [[nodiscard]] virtual DatagramSourceReceiveResult receive_for(
        std::chrono::milliseconds timeout) = 0;

    virtual void close() noexcept = 0;

protected:
    DatagramSourceBackend() = default;
};

}  // namespace semilive::receiver::contracts::network
