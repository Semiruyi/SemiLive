#include "publisher/domain/resource/captured_frame_store/captured_frame_store.hpp"

#include "publisher/domain/resource/captured_frame_store/captured_frame_store_events.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace semilive::publisher::domain {

CapturedFrameStore::CapturedFrameStore(std::shared_ptr<infra::Notifier> notifier,
                                       const std::size_t capacity)
    : notifier_(std::move(notifier)), capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("CapturedFrameStore capacity must be greater than zero");
    }
}

CapturedFramePushResult CapturedFrameStore::try_push(CapturedFrame&& frame) {
    CapturedFramePushResult result = CapturedFramePushResult::Accepted;
    bool should_notify_not_empty = false;
    {
        std::lock_guard lock{mutex_};
        if (frames_.size() == capacity_) {
            frames_.pop_front();
            ++replaced_count_;
            result = CapturedFramePushResult::ReplacedOldest;
        }

        should_notify_not_empty = frames_.empty();
        frames_.push_back(std::move(frame));
        peak_size_ = std::max(peak_size_, frames_.size());
    }
    if (should_notify_not_empty) {
        notify_not_empty();
    }
    return result;
}

std::optional<CapturedFrame> CapturedFrameStore::try_pop() {
    std::lock_guard lock{mutex_};
    if (frames_.empty()) {
        return std::nullopt;
    }

    CapturedFrame frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
}

bool CapturedFrameStore::empty() const noexcept {
    std::lock_guard lock{mutex_};
    return frames_.empty();
}

std::size_t CapturedFrameStore::clear() noexcept {
    std::lock_guard lock{mutex_};
    const std::size_t removed = frames_.size();
    frames_.clear();
    return removed;
}

std::size_t CapturedFrameStore::size() const noexcept {
    std::lock_guard lock{mutex_};
    return frames_.size();
}

std::size_t CapturedFrameStore::capacity() const noexcept {
    return capacity_;
}

std::size_t CapturedFrameStore::peak_size() const noexcept {
    std::lock_guard lock{mutex_};
    return peak_size_;
}

std::uint64_t CapturedFrameStore::replaced_count() const noexcept {
    std::lock_guard lock{mutex_};
    return replaced_count_;
}

void CapturedFrameStore::notify_not_empty() noexcept {
    if (!notifier_) {
        return;
    }
    try {
        (void)notifier_->send(CapturedFrameStoreNotEmpty{});
    } catch (...) {
    }
}

}  // namespace semilive::publisher::domain
