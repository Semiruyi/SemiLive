#include <semilive/publisher/infrastructure/output/rtp_udp_video_output_backend.hpp>

#include "h264_rtp_packetizer.hpp"
#include "udp_socket.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

namespace semilive::publisher::infra::output {
namespace {

using contracts::output::VideoOutputIssue;
using contracts::output::VideoOutputOperation;

VideoOutputIssue issue(const VideoOutputOperation operation,
                       const std::int64_t native_code,
                       std::string message) {
    return {operation, native_code, std::move(message)};
}

[[nodiscard]] std::uint16_t read_u16(
    const std::span<const std::byte> bytes) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(bytes[0]))
         << 8U) |
        std::to_integer<std::uint8_t>(bytes[1]));
}

[[nodiscard]] std::uint32_t read_u32(
    const std::span<const std::byte> bytes) noexcept {
    return (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(bytes[0]))
            << 24U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(bytes[1]))
            << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(bytes[2]))
            << 8U) |
           std::to_integer<std::uint8_t>(bytes[3]);
}

using SessionStateResult =
    std::expected<detail::RtpSessionState, VideoOutputIssue>;

[[nodiscard]] SessionStateResult random_session_state() {
    std::array<std::byte, 10> random_bytes{};
#if defined(_WIN32)
    const auto status = BCryptGenRandom(
        nullptr, reinterpret_cast<PUCHAR>(random_bytes.data()),
        static_cast<ULONG>(random_bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        return std::unexpected{issue(
            VideoOutputOperation::Open, static_cast<std::int64_t>(status),
            "failed to generate RTP session state")};
    }
#else
    std::size_t offset = 0;
    while (offset < random_bytes.size()) {
        const auto received = getrandom(random_bytes.data() + offset,
                                        random_bytes.size() - offset, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected{issue(
                VideoOutputOperation::Open,
                static_cast<std::int64_t>(errno),
                "failed to generate RTP session state")};
        }
        if (received == 0) {
            return std::unexpected{issue(
                VideoOutputOperation::Open, 0,
                "system random source returned no RTP session bytes")};
        }
        offset += static_cast<std::size_t>(received);
    }
#endif

    return detail::RtpSessionState{
        read_u16(std::span{random_bytes}.subspan<0, 2>()),
        read_u32(std::span{random_bytes}.subspan<2, 4>()),
        read_u32(std::span{random_bytes}.subspan<6, 4>()),
    };
}

[[nodiscard]] std::string output_name(
    const RtpUdpVideoOutputConfig& config) {
    std::string result{"rtp+udp://"};
    const bool ipv6 = config.destination_address.find(':') !=
                      std::string::npos;
    if (ipv6) {
        result += '[';
    }
    result += config.destination_address;
    if (ipv6) {
        result += ']';
    }
    result += ':';
    result += std::to_string(config.destination_port);
    result += "?pt=";
    result += std::to_string(config.payload_type);
    return result;
}

}  // namespace

struct RtpUdpVideoOutputBackend::Impl {
    enum class State : std::uint8_t {
        Closed,
        Open,
        Flushed,
        Failed,
    };

    explicit Impl(RtpUdpVideoOutputConfig config)
        : config{std::move(config)} {}

    [[nodiscard]] contracts::output::VideoOutputOpenResult open();
    [[nodiscard]] contracts::output::VideoOutputConsumeResult consume(
        const model::EncodedVideoAccessUnit& access_unit);
    [[nodiscard]] contracts::output::VideoOutputFlushResult flush();
    void close() noexcept;

    RtpUdpVideoOutputConfig config;
    detail::UdpSocket socket;
    std::unique_ptr<detail::H264RtpPacketizer> packetizer;
    detail::RtpSessionState session;
    State state = State::Closed;
};

contracts::output::VideoOutputOpenResult
RtpUdpVideoOutputBackend::Impl::open() {
    if (state != State::Closed) {
        return std::unexpected{issue(
            VideoOutputOperation::State, 0,
            "RTP/UDP video output backend can only open from the closed state")};
    }

    const detail::H264RtpPacketizerConfig packetizer_config{
        config.payload_type, config.max_datagram_bytes};
    if (const auto valid =
            detail::validate_h264_rtp_packetizer_config(packetizer_config);
        !valid) {
        return std::unexpected{
            issue(VideoOutputOperation::Open, 0, valid.error())};
    }

    auto name = output_name(config);
    auto new_packetizer =
        std::make_unique<detail::H264RtpPacketizer>(packetizer_config);

    if (auto opened = socket.open(config.destination_address,
                                  config.destination_port);
        !opened) {
        return std::unexpected{issue(VideoOutputOperation::Open,
                                     opened.error().native_code,
                                     std::move(opened.error().message))};
    }

    auto random_state = random_session_state();
    if (!random_state) {
        socket.close();
        return std::unexpected{std::move(random_state.error())};
    }
    session = *random_state;

    packetizer = std::move(new_packetizer);
    state = State::Open;
    return contracts::output::VideoOutputInfo{std::move(name)};
}

contracts::output::VideoOutputConsumeResult
RtpUdpVideoOutputBackend::Impl::consume(
    const model::EncodedVideoAccessUnit& access_unit) {
    if (state != State::Open) {
        return std::unexpected{issue(
            VideoOutputOperation::State, 0,
            "RTP/UDP video output backend must be open before consuming")};
    }

    std::optional<detail::UdpSocketIssue> send_issue;
    std::uint64_t sent_datagrams = 0;
    auto packetized = packetizer->packetize(
        access_unit.annex_b, access_unit.presentation_time, session,
        [this, &send_issue, &sent_datagrams](
            const std::span<const std::byte> datagram)
            -> detail::RtpDatagramEmitResult {
            auto sent = socket.send(datagram);
            if (!sent) {
                send_issue = std::move(sent.error());
                return std::unexpected{send_issue->message};
            }
            ++sent_datagrams;
            return {};
        });
    if (!packetized) {
        state = State::Failed;
        const auto native_code =
            send_issue ? send_issue->native_code : 0;
        auto message = std::move(packetized.error());
        message += " after ";
        message += std::to_string(sent_datagrams);
        message += " RTP datagram(s)";
        return std::unexpected{issue(VideoOutputOperation::Consume,
                                     native_code, std::move(message))};
    }

    return contracts::output::VideoOutputReceipt{
        packetized->emitted_datagrams, packetized->emitted_bytes};
}

contracts::output::VideoOutputFlushResult
RtpUdpVideoOutputBackend::Impl::flush() {
    if (state != State::Open) {
        return std::unexpected{issue(
            VideoOutputOperation::State, 0,
            "RTP/UDP video output backend can only flush an open session once")};
    }
    state = State::Flushed;
    return contracts::output::VideoOutputReceipt{};
}

void RtpUdpVideoOutputBackend::Impl::close() noexcept {
    socket.close();
    packetizer.reset();
    state = State::Closed;
}

RtpUdpVideoOutputBackend::RtpUdpVideoOutputBackend(
    RtpUdpVideoOutputConfig config)
    : impl_{std::make_unique<Impl>(std::move(config))} {}

RtpUdpVideoOutputBackend::~RtpUdpVideoOutputBackend() {
    impl_->close();
}

contracts::output::VideoOutputOpenResult
RtpUdpVideoOutputBackend::open() {
    return impl_->open();
}

contracts::output::VideoOutputConsumeResult
RtpUdpVideoOutputBackend::consume(
    const model::EncodedVideoAccessUnit& access_unit) {
    return impl_->consume(access_unit);
}

contracts::output::VideoOutputFlushResult
RtpUdpVideoOutputBackend::flush() {
    return impl_->flush();
}

void RtpUdpVideoOutputBackend::close() noexcept {
    impl_->close();
}

}  // namespace semilive::publisher::infra::output
