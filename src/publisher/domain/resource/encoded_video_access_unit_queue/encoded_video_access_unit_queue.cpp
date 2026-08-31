#include "publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue.hpp"

#include "publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue_events.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace semilive::publisher::domain {

EncodedVideoAccessUnitQueue::EncodedVideoAccessUnitQueue(std::shared_ptr<infra::Notifier> notifier,
                                               const std::size_t capacity)
    : notifier_(std::move(notifier)), capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("EncodedVideoAccessUnitQueue capacity must be greater than zero");
    }
}

EncodedVideoAccessUnitPushResult EncodedVideoAccessUnitQueue::try_push(EncodedVideoAccessUnit&& access_unit) {
    bool should_notify_not_empty = false;
    {
        std::lock_guard lock{mutex_};
        if (access_units_.size() == capacity_) {
            return EncodedVideoAccessUnitPushResult::Full;
        }

        should_notify_not_empty = access_units_.empty();
        access_units_.push_back(std::move(access_unit));
        peak_size_ = std::max(peak_size_, access_units_.size());
    }
    if (should_notify_not_empty) {
        notify_not_empty();
    }
    return EncodedVideoAccessUnitPushResult::Accepted;
}

bool EncodedVideoAccessUnitQueue::full() const noexcept {
    std::lock_guard lock{mutex_};
    return access_units_.size() == capacity_;
}

std::optional<EncodedVideoAccessUnit> EncodedVideoAccessUnitQueue::try_pop() {
    std::optional<EncodedVideoAccessUnit> result;
    bool should_notify_not_full = false;
    {
        std::lock_guard lock{mutex_};
        if (access_units_.empty()) {
            return std::nullopt;
        }

        should_notify_not_full = access_units_.size() == capacity_;
        result.emplace(std::move(access_units_.front()));
        access_units_.pop_front();
    }
    if (should_notify_not_full) {
        notify_not_full();
    }
    return result;
}

bool EncodedVideoAccessUnitQueue::empty() const noexcept {
    std::lock_guard lock{mutex_};
    return access_units_.empty();
}

std::size_t EncodedVideoAccessUnitQueue::clear() noexcept {
    bool should_notify_not_full = false;
    std::size_t removed = 0;
    {
        std::lock_guard lock{mutex_};
        should_notify_not_full = access_units_.size() == capacity_;
        removed = access_units_.size();
        access_units_.clear();
    }
    if (should_notify_not_full) {
        notify_not_full();
    }
    return removed;
}

std::size_t EncodedVideoAccessUnitQueue::size() const noexcept {
    std::lock_guard lock{mutex_};
    return access_units_.size();
}

std::size_t EncodedVideoAccessUnitQueue::capacity() const noexcept {
    return capacity_;
}

std::size_t EncodedVideoAccessUnitQueue::peak_size() const noexcept {
    std::lock_guard lock{mutex_};
    return peak_size_;
}

void EncodedVideoAccessUnitQueue::notify_not_empty() noexcept {
    if (!notifier_) {
        return;
    }
    try {
        (void)notifier_->send(EncodedVideoAccessUnitQueueNotEmpty{});
    } catch (...) {
    }
}

void EncodedVideoAccessUnitQueue::notify_not_full() noexcept {
    if (!notifier_) {
        return;
    }
    try {
        (void)notifier_->send(EncodedVideoAccessUnitQueueNotFull{});
    } catch (...) {
    }
}

}  // namespace semilive::publisher::domain
