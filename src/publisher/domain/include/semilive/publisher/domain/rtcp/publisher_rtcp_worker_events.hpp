#pragma once

#include <semilive/publisher/domain/rtcp/publisher_rtcp_worker.hpp>

namespace semilive::publisher::domain {

struct PublisherRtcpWorkerFailed {
    PublisherRtcpWorkerIssue issue;
};

}  // namespace semilive::publisher::domain
