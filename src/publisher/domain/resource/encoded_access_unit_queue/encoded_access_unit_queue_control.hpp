#pragma once

#include <cstddef>

namespace semilive::publisher::domain {

class EncodedAccessUnitQueueControl {
public:
    virtual ~EncodedAccessUnitQueueControl() = default;

    EncodedAccessUnitQueueControl(const EncodedAccessUnitQueueControl&) = delete;
    EncodedAccessUnitQueueControl& operator=(const EncodedAccessUnitQueueControl&) = delete;
    EncodedAccessUnitQueueControl(EncodedAccessUnitQueueControl&&) = delete;
    EncodedAccessUnitQueueControl& operator=(EncodedAccessUnitQueueControl&&) = delete;

    [[nodiscard]] virtual std::size_t clear() noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual std::size_t capacity() const noexcept = 0;
    [[nodiscard]] virtual std::size_t peak_size() const noexcept = 0;

protected:
    EncodedAccessUnitQueueControl() = default;
};

}  // namespace semilive::publisher::domain
