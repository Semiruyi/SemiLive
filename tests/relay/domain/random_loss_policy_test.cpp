#include <semilive/relay/domain/random_loss_policy.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace domain = semilive::relay::domain;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

void honors_loss_boundaries() {
    domain::RandomLossPolicy none{0, 7};
    domain::RandomLossPolicy all{domain::RandomLossPolicy::rate_scale, 7};
    for (std::size_t index = 0; index < 1'000; ++index) {
        require(!none.should_drop(), "zero loss must forward every packet");
        require(all.should_drop(), "full loss must drop every packet");
    }
}

void produces_a_repeatable_sequence() {
    domain::RandomLossPolicy first{10'000, 20260919};
    domain::RandomLossPolicy second{10'000, 20260919};
    domain::RandomLossPolicy other{10'000, 20260920};

    std::vector<bool> first_sequence;
    std::vector<bool> second_sequence;
    std::vector<bool> other_sequence;
    for (std::size_t index = 0; index < 10'000; ++index) {
        first_sequence.push_back(first.should_drop());
        second_sequence.push_back(second.should_drop());
        other_sequence.push_back(other.should_drop());
    }
    require(first_sequence == second_sequence,
            "equal seeds must produce equal drop decisions");
    require(first_sequence != other_sequence,
            "different seeds should produce different drop decisions");
}

void rejects_an_invalid_rate() {
    bool rejected = false;
    try {
        const domain::RandomLossPolicy invalid{
            domain::RandomLossPolicy::rate_scale + 1U, 1};
        static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "loss rates above 100 percent must be rejected");
}

}  // namespace

int main() {
    try {
        honors_loss_boundaries();
        produces_a_repeatable_sequence();
        rejects_an_invalid_rate();
    } catch (const std::exception& error) {
        std::cerr << "random loss policy test failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
