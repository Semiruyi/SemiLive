#pragma once

#include "publisher/domain/worker/video_capture_worker/video_capture_worker.hpp"

namespace semilive::publisher::domain {

struct VideoCaptureWorkerFailed {
    VideoCaptureWorkerIssue issue;
};

}  // namespace semilive::publisher::domain
