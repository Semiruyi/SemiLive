#pragma once

#include "publisher/domain/resource/captured_frame_store/captured_frame_sink.hpp"
#include "publisher/domain/resource/captured_frame_store/captured_frame_source.hpp"
#include "publisher/domain/resource/captured_frame_store/captured_frame_store_control.hpp"
#include "publisher/infrastructure/notifier/notifier.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>

namespace semilive::publisher::domain {

class CapturedFrameStore final : public CapturedFrameSink,
                                 public CapturedFrameSource,
                                 public CapturedFrameStoreControl {
public:
    static constexpr std::size_t kDefaultCapacity = 2;

    explicit CapturedFrameStore(std::shared_ptr<infra::Notifier> notifier,
                                std::size_t capacity = kDefaultCapacity);
    ~CapturedFrameStore() override = default;

    [[nodiscard]] CapturedFramePushResult try_push(CapturedFrame&& frame) override;
    [[nodiscard]] std::optional<CapturedFrame> try_pop() override;
    [[nodiscard]] bool empty() const noexcept override;
    [[nodiscard]] std::size_t clear() noexcept override;
    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::size_t capacity() const noexcept override;
    [[nodiscard]] std::size_t peak_size() const noexcept override;
    [[nodiscard]] std::uint64_t replaced_count() const noexcept override;

private:
    void notify_not_empty() noexcept;

    std::shared_ptr<infra::Notifier> notifier_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<CapturedFrame> frames_;
    std::size_t peak_size_ = 0;
    std::uint64_t replaced_count_ = 0;
};

}  // namespace semilive::publisher::domain
