#pragma once

#include <semilive/publisher/contracts/output/video_access_unit_output_backend.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace semilive::publisher::infra::output {

struct RtpUdpVideoOutputConfig {
    std::string destination_address;
    std::uint16_t destination_port = 0;
    std::uint8_t payload_type = 96;
    std::size_t max_datagram_bytes = 1200;
};

class RtpUdpVideoOutputBackend final
    : public contracts::output::VideoAccessUnitOutputBackend {
public:
    explicit RtpUdpVideoOutputBackend(RtpUdpVideoOutputConfig config);
    ~RtpUdpVideoOutputBackend() override;

    RtpUdpVideoOutputBackend(const RtpUdpVideoOutputBackend&) = delete;
    RtpUdpVideoOutputBackend& operator=(const RtpUdpVideoOutputBackend&) =
        delete;
    RtpUdpVideoOutputBackend(RtpUdpVideoOutputBackend&&) = delete;
    RtpUdpVideoOutputBackend& operator=(RtpUdpVideoOutputBackend&&) = delete;

    [[nodiscard]] contracts::output::VideoOutputOpenResult open() override;
    [[nodiscard]] contracts::output::VideoOutputConsumeResult consume(
        const model::EncodedVideoAccessUnit& access_unit) override;
    [[nodiscard]] contracts::output::VideoOutputFlushResult flush() override;
    void close() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::publisher::infra::output
