#include <semilive/relay/domain/random_loss_policy.hpp>

#include <stdexcept>

namespace semilive::relay::domain {

RandomLossPolicy::RandomLossPolicy(const std::uint32_t loss_rate_ppm,
                                   const std::uint64_t seed)
    : loss_rate_ppm_{loss_rate_ppm}, seed_{seed}, random_{seed} {
    if (loss_rate_ppm > rate_scale) {
        throw std::invalid_argument{
            "random loss rate must be in 0..1000000 ppm"};
    }
}

bool RandomLossPolicy::should_drop() noexcept {
    const auto sample = random_() % rate_scale;
    return sample < loss_rate_ppm_;
}

std::uint32_t RandomLossPolicy::loss_rate_ppm() const noexcept {
    return loss_rate_ppm_;
}

std::uint64_t RandomLossPolicy::seed() const noexcept {
    return seed_;
}

}  // namespace semilive::relay::domain
