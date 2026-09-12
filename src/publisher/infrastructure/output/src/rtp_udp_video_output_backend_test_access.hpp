#pragma once

#include <cstdint>

namespace semilive::publisher::infra::output {

class RtpUdpVideoOutputBackend;

namespace detail {

class RtpUdpVideoOutputBackendTestAccess {
public:
    static void set_next_session_state(
        RtpUdpVideoOutputBackend& backend,
        std::uint16_t next_sequence,
        std::uint32_t initial_timestamp,
        std::uint32_t ssrc);
};

}  // namespace detail
}  // namespace semilive::publisher::infra::output
