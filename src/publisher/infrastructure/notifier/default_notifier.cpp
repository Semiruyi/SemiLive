#include "publisher/infrastructure/notifier/default_notifier.hpp"

#include "common/infrastructure/log/log.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#define SEMILIVE_LOG_TAG "notifier"

namespace semilive::publisher::infra {

struct DefaultNotifier::Slot {
    explicit Slot(std::function<void(const void*)> callback) : callback(std::move(callback)) {}

    std::atomic_bool active{true};
    std::function<void(const void*)> callback;
};

struct DefaultNotifier::State {
    std::mutex mutex;
    std::unordered_map<std::type_index, std::vector<std::shared_ptr<Slot>>> slots;
};

DefaultNotifier::SubscriptionImpl::SubscriptionImpl(std::weak_ptr<State> state,
                                                     const std::type_index type,
                                                     std::shared_ptr<Slot> slot)
    : state_(std::move(state)), type_(type), slot_(std::move(slot)) {}

DefaultNotifier::SubscriptionImpl::~SubscriptionImpl() {
    (void)unsubscribe();
}

bool DefaultNotifier::SubscriptionImpl::unsubscribe() noexcept {
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

    auto& callbacks = iterator->second;
    const auto new_end = std::remove(callbacks.begin(), callbacks.end(), slot_);
    const bool removed = new_end != callbacks.end();
    callbacks.erase(new_end, callbacks.end());
    if (callbacks.empty()) {
        state->slots.erase(iterator);
    }
    return removed;
}

bool DefaultNotifier::SubscriptionImpl::active() const noexcept {
    return slot_ && slot_->active.load(std::memory_order_acquire);
}

DefaultNotifier::DefaultNotifier() : state_(std::make_shared<State>()) {}

DefaultNotifier::~DefaultNotifier() {
    (void)clear_all();
}

std::shared_ptr<Notifier::Subscription> DefaultNotifier::subscribe_erased(
    const std::type_index type,
    std::function<void(const void*)> callback) {
    auto slot = std::make_shared<Slot>(std::move(callback));
    {
        std::lock_guard lock{state_->mutex};
        state_->slots[type].push_back(slot);
    }
    return std::make_shared<SubscriptionImpl>(state_, type, std::move(slot));
}

bool DefaultNotifier::send_erased(const std::type_index type, const void* event) {
    std::vector<std::shared_ptr<Slot>> snapshot;
    {
        std::lock_guard lock{state_->mutex};
        const auto iterator = state_->slots.find(type);
        if (iterator == state_->slots.end()) {
            return false;
        }
        snapshot = iterator->second;
    }

    bool dispatched = false;
    for (const auto& slot : snapshot) {
        if (!slot->active.load(std::memory_order_acquire)) {
            continue;
        }
        dispatched = true;
        try {
            slot->callback(event);
        } catch (const std::exception& exception) {
            SEMILIVE_LOG_ERROR(
                "callback threw for event type {}: {}", type.name(), exception.what());
        } catch (...) {
            SEMILIVE_LOG_ERROR("callback threw for event type {}: unknown exception", type.name());
        }
    }
    return dispatched;
}

bool DefaultNotifier::clear_all() noexcept {
    std::vector<std::shared_ptr<Slot>> removed;
    {
        std::lock_guard lock{state_->mutex};
        for (auto& [type, callbacks] : state_->slots) {
            (void)type;
            removed.insert(removed.end(), callbacks.begin(), callbacks.end());
        }
        state_->slots.clear();
    }

    for (const auto& slot : removed) {
        slot->active.store(false, std::memory_order_release);
    }
    return !removed.empty();
}

}  // namespace semilive::publisher::infra
