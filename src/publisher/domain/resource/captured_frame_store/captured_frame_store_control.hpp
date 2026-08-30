#pragma once

#include <cstddef>
#include <cstdint>

namespace semilive::publisher::domain {

class CapturedFrameStoreControl {
public:
    virtual ~CapturedFrameStoreControl() = default;

    CapturedFrameStoreControl(const CapturedFrameStoreControl&) = delete;
    CapturedFrameStoreControl& operator=(const CapturedFrameStoreControl&) = delete;
    CapturedFrameStoreControl(CapturedFrameStoreControl&&) = delete;
    CapturedFrameStoreControl& operator=(CapturedFrameStoreControl&&) = delete;

    [[nodiscard]] virtual std::size_t clear() noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual std::size_t capacity() const noexcept = 0;
    [[nodiscard]] virtual std::size_t peak_size() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t replaced_count() const noexcept = 0;

protected:
    CapturedFrameStoreControl() = default;
};

}  // namespace semilive::publisher::domain
