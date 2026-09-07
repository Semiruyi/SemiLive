#pragma once

#include <cstddef>

namespace semilive::publisher::domain {

class EncodedVideoAccessUnitQueueControl {
public:
    virtual ~EncodedVideoAccessUnitQueueControl() = default;

    EncodedVideoAccessUnitQueueControl(const EncodedVideoAccessUnitQueueControl&) = delete;
    EncodedVideoAccessUnitQueueControl& operator=(const EncodedVideoAccessUnitQueueControl&) = delete;
    EncodedVideoAccessUnitQueueControl(EncodedVideoAccessUnitQueueControl&&) = delete;
    EncodedVideoAccessUnitQueueControl& operator=(EncodedVideoAccessUnitQueueControl&&) = delete;

    [[nodiscard]] virtual std::size_t clear() noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual std::size_t capacity() const noexcept = 0;
    [[nodiscard]] virtual std::size_t peak_size() const noexcept = 0;

protected:
    EncodedVideoAccessUnitQueueControl() = default;
};

}  // namespace semilive::publisher::domain
