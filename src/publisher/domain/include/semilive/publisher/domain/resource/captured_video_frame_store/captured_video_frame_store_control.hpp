#pragma once

#include <cstddef>
#include <cstdint>

namespace semilive::publisher::domain {

class CapturedVideoFrameStoreControl {
public:
    virtual ~CapturedVideoFrameStoreControl() = default;

    CapturedVideoFrameStoreControl(const CapturedVideoFrameStoreControl&) = delete;
    CapturedVideoFrameStoreControl& operator=(const CapturedVideoFrameStoreControl&) = delete;
    CapturedVideoFrameStoreControl(CapturedVideoFrameStoreControl&&) = delete;
    CapturedVideoFrameStoreControl& operator=(CapturedVideoFrameStoreControl&&) = delete;

    [[nodiscard]] virtual std::size_t clear() noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual std::size_t capacity() const noexcept = 0;
    [[nodiscard]] virtual std::size_t peak_size() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t replaced_count() const noexcept = 0;

protected:
    CapturedVideoFrameStoreControl() = default;
};

}  // namespace semilive::publisher::domain
