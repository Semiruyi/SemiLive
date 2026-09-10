#pragma once

#include <semilive/publisher/domain/worker/video_output_worker/video_output_worker.hpp>

namespace semilive::publisher::domain {

struct VideoOutputWorkerFailed {
    VideoOutputWorkerIssue issue;
};

}  // namespace semilive::publisher::domain
