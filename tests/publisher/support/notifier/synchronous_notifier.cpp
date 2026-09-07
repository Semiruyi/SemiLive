#include "publisher/support/notifier/synchronous_notifier.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace semilive::publisher::test_support {

struct SynchronousNotifier::Slot {
    explicit Slot(std::function<void(const void*)> callback)
        : callback{std::move(callback)} {}

    std::atomic_bool active{true};
    std::function<void(const void*)> callback;
};

struct SynchronousNotifier::State {
    std::mutex mutex;
    std::unordered_map<std::type_index, std::vector<std::shared_ptr<Slot>>> slots;
};

SynchronousNotifier::SubscriptionImpl::SubscriptionImpl(
    std::weak_ptr<State> state,
    const std::type_index type,
    std::shared_ptr<Slot> slot)
    : state_{std::move(state)}, type_{type}, slot_{std::move(slot)} {}

SynchronousNotifier::SubscriptionImpl::~SubscriptionImpl() {
    (void)unsubscribe();
}

bool SynchronousNotifier::SubscriptionImpl::unsubscribe() noexcept {
    if (!slot_ || !slot_->active.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    const auto state = state_.lock();
    if (!state) {
        return false;
    }

    std::lock_guard lock{state->mutex};
    const auto iterator = state->slots.find(type_);
    if (iterator == state->slots.end()) {
        return false;
    }
    auto& slots = iterator->second;
    const auto new_end = std::remove(slots.begin(), slots.end(), slot_);
    const bool removed = new_end != slots.end();
    slots.erase(new_end, slots.end());
    if (slots.empty()) {
        state->slots.erase(iterator);
    }
    return removed;
}

bool SynchronousNotifier::SubscriptionImpl::active() const noexcept {
    return slot_ && slot_->active.load(std::memory_order_acquire);
}

SynchronousNotifier::SynchronousNotifier() : state_{std::make_shared<State>()} {}

SynchronousNotifier::~SynchronousNotifier() {
    clear();
}

std::shared_ptr<contracts::Notifier::Subscription>
SynchronousNotifier::subscribe_erased(
    const std::type_index type,
    std::function<void(const void*)> callback) {
    auto slot = std::make_shared<Slot>(std::move(callback));
    {
        std::lock_guard lock{state_->mutex};
        state_->slots[type].push_back(slot);
    }
    return std::make_shared<SubscriptionImpl>(state_, type, std::move(slot));
}

bool SynchronousNotifier::send_erased(const std::type_index type,
                                      const void* event) {
    std::vector<std::shared_ptr<Slot>> slots;
    {
        std::lock_guard lock{state_->mutex};
        const auto iterator = state_->slots.find(type);
        if (iterator == state_->slots.end()) {
            return false;
        }
        slots = iterator->second;
    }

    bool dispatched = false;
    for (const auto& slot : slots) {
        if (!slot->active.load(std::memory_order_acquire)) {
            continue;
        }
        dispatched = true;
        try {
            slot->callback(event);
        } catch (...) {
        }
    }
    return dispatched;
}

void SynchronousNotifier::clear() noexcept {
    std::vector<std::shared_ptr<Slot>> slots;
    {
        std::lock_guard lock{state_->mutex};
        for (auto& [type, registered] : state_->slots) {
            (void)type;
            slots.insert(slots.end(), registered.begin(), registered.end());
        }
        state_->slots.clear();
    }
    for (const auto& slot : slots) {
        slot->active.store(false, std::memory_order_release);
    }
}

}  // namespace semilive::publisher::test_support
