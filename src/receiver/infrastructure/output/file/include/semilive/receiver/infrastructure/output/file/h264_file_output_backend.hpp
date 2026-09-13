#pragma once

#include <semilive/receiver/contracts/output/live_video_output_backend.hpp>

#include <filesystem>
#include <memory>

namespace semilive::receiver::infra::output {

class H264FileOutputBackend final
    : public contracts::output::LiveVideoOutputBackend {
public:
    explicit H264FileOutputBackend(std::filesystem::path output_path);
    ~H264FileOutputBackend() override;

    H264FileOutputBackend(const H264FileOutputBackend&) = delete;
    H264FileOutputBackend& operator=(const H264FileOutputBackend&) = delete;
    H264FileOutputBackend(H264FileOutputBackend&&) = delete;
    H264FileOutputBackend& operator=(H264FileOutputBackend&&) = delete;

    [[nodiscard]] contracts::output::LiveVideoOutputOpenResult open() override;
    [[nodiscard]] contracts::output::LiveVideoOutputSubmitResult submit(
        model::TimedH264AccessUnit access_unit) override;
    void close(contracts::output::LiveVideoOutputCloseMode mode) noexcept
        override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace semilive::receiver::infra::output
