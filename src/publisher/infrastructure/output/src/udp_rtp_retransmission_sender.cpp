#include <semilive/publisher/infrastructure/output/udp_rtp_retransmission_sender.hpp>

#include "udp_socket.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace semilive::publisher::infra::output {
namespace {

contracts::output::RtpRetransmissionSenderIssue sender_issue(
    detail::UdpSocketIssue issue) {
    return {issue.native_code, std::move(issue.message)};
}

}  // namespace

struct UdpRtpRetransmissionSender::Impl {
    explicit Impl(UdpRtpRetransmissionSenderConfig config)
        : config{std::move(config)} {}

    UdpRtpRetransmissionSenderConfig config;
    detail::UdpSocket socket;
};

UdpRtpRetransmissionSender::UdpRtpRetransmissionSender(
    UdpRtpRetransmissionSenderConfig config)
    : impl_{std::make_unique<Impl>(std::move(config))} {}

UdpRtpRetransmissionSender::~UdpRtpRetransmissionSender() {
    impl_->socket.close();
}

contracts::output::RtpRetransmissionSenderResult
UdpRtpRetransmissionSender::open() {
    auto opened = impl_->socket.open(impl_->config.destination_address,
                                     impl_->config.destination_port);
    if (!opened) {
        return std::unexpected{sender_issue(std::move(opened.error()))};
    }
    return {};
}

contracts::output::RtpRetransmissionSenderResult
UdpRtpRetransmissionSender::send(
    const std::span<const std::byte> datagram) {
    auto sent = impl_->socket.send(datagram);
    if (!sent) {
        return std::unexpected{sender_issue(std::move(sent.error()))};
    }
    return {};
}

void UdpRtpRetransmissionSender::close() noexcept {
    impl_->socket.close();
}

}  // namespace semilive::publisher::infra::output
