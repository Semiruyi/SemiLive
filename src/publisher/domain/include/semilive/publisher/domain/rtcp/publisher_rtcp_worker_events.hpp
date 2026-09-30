#pragma once

#include <semilive/publisher/domain/rtcp/publisher_rtcp_worker.hpp>

namespace semilive::publisher::domain {

struct PublisherRtcpWorkerFailed {
    PublisherRtcpWorkerIssue issue;
};

struct PublisherPictureLossIndicationReceived {
    std::uint32_t sender_ssrc = 0;
    std::uint32_t media_source_ssrc = 0;
};

}  // namespace semilive::publisher::domain
