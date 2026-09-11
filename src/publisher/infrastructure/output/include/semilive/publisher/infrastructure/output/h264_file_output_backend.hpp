#pragma once

#include <semilive/publisher/contracts/output/video_access_unit_output_backend.hpp>

#include <filesystem>
#include <memory>

namespace semilive::publisher::infra::output {

class H264FileOutputBackend final
    : public contracts::output::VideoAccessUnitOutputBackend {
public:
    explicit H264FileOutputBackend(std::filesystem::path output_path);
    ~H264FileOutputBackend() override;

    H264FileOutputBackend(const H264FileOutputBackend&) = delete;
    H264FileOutputBackend& operator=(const H264FileOutputBackend&) = delete;
    H264FileOutputBackend(H264FileOutputBackend&&) = delete;
    H264FileOutputBackend& operator=(H264FileOutputBackend&&) = delete;

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
