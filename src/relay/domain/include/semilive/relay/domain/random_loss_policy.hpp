#pragma once

#include <cstdint>
#include <random>

namespace semilive::relay::domain {

class RandomLossPolicy final {
public:
    static constexpr std::uint32_t rate_scale = 1'000'000;

    RandomLossPolicy(std::uint32_t loss_rate_ppm, std::uint64_t seed);

    [[nodiscard]] bool should_drop() noexcept;
    [[nodiscard]] std::uint32_t loss_rate_ppm() const noexcept;
    [[nodiscard]] std::uint64_t seed() const noexcept;

private:
    std::uint32_t loss_rate_ppm_ = 0;
    std::uint64_t seed_ = 0;
    std::mt19937_64 random_;
};

}  // namespace semilive::relay::domain
