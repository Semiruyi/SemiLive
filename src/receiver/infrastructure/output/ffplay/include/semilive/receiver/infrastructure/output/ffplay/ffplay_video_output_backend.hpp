#pragma once

#include <semilive/receiver/contracts/output/live_video_output_backend.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace semilive::receiver::infra::output {

struct FfplayVideoOutputConfig {
    std::filesystem::path executable_path{"ffplay"};
    std::vector<std::string> arguments{
        "-hide_banner",
        "-loglevel",
        "warning",
        "-f",
        "h264",
        "-fflags",
        "nobuffer",
        "-flags",
        "low_delay",
        "-analyzeduration",
        "0",
        "-probesize",
        "32",
        "-an",
        "-sn",
        "-i",
        "pipe:0",
    };
    std::size_t maximum_buffered_access_units = 32;
    std::size_t maximum_buffered_bytes = 4U * 1024U * 1024U;
};

class FfplayVideoOutputBackend final
    : public contracts::output::LiveVideoOutputBackend {
public:
    explicit FfplayVideoOutputBackend(FfplayVideoOutputConfig config = {});
    ~FfplayVideoOutputBackend() override;

    FfplayVideoOutputBackend(const FfplayVideoOutputBackend&) = delete;
    FfplayVideoOutputBackend& operator=(const FfplayVideoOutputBackend&) =
        delete;
    FfplayVideoOutputBackend(FfplayVideoOutputBackend&&) = delete;
    FfplayVideoOutputBackend& operator=(FfplayVideoOutputBackend&&) = delete;

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
