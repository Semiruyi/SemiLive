#pragma once

#include "publisher/domain/resource/captured_video_frame_store/captured_video_frame_sink.hpp"
#include "publisher/domain/resource/captured_video_frame_store/captured_video_frame_source.hpp"
#include "publisher/domain/resource/captured_video_frame_store/captured_video_frame_store_control.hpp"
#include "publisher/infrastructure/notifier/notifier.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>

namespace semilive::publisher::domain {

class CapturedVideoFrameStore final : public CapturedVideoFrameSink,
                                 public CapturedVideoFrameSource,
                                 public CapturedVideoFrameStoreControl {
public:
    static constexpr std::size_t kDefaultCapacity = 2;

    explicit CapturedVideoFrameStore(std::shared_ptr<infra::Notifier> notifier,
                                std::size_t capacity = kDefaultCapacity);
    ~CapturedVideoFrameStore() override = default;

    [[nodiscard]] CapturedVideoFramePushResult try_push(
        model::CapturedVideoFrame&& frame) override;
    [[nodiscard]] std::optional<model::CapturedVideoFrame> try_pop() override;
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
    std::deque<model::CapturedVideoFrame> frames_;
    std::size_t peak_size_ = 0;
    std::uint64_t replaced_count_ = 0;
};

}  // namespace semilive::publisher::domain
