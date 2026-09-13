#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

namespace semilive::receiver::model {

struct UdpDatagram {
    using Clock = std::chrono::steady_clock;

    std::vector<std::byte> bytes;
    Clock::time_point received_at{};
};

}  // namespace semilive::receiver::model
