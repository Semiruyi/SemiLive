#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace semilive::receiver::model {

struct UdpDatagram {
    using Clock = std::chrono::steady_clock;

    std::vector<std::byte> bytes;
    Clock::time_point received_at{};
    std::int64_t measurement_received = 0;
};

}  // namespace semilive::receiver::model
