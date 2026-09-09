#pragma once

#include <semilive/publisher/domain/worker/video_encoder_worker/video_encoder_worker.hpp>

namespace semilive::publisher::domain {

struct VideoEncoderWorkerFailed {
    VideoEncoderWorkerIssue issue;
};

}  // namespace semilive::publisher::domain
