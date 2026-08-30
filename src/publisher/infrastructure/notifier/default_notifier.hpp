#pragma once

#include "publisher/infrastructure/notifier/notifier.hpp"

#include <functional>
#include <memory>
#include <typeindex>

namespace semilive::publisher::infra {

class DefaultNotifier final : public Notifier {
public:
    DefaultNotifier();
    ~DefaultNotifier() override;

    DefaultNotifier(const DefaultNotifier&) = delete;
    DefaultNotifier& operator=(const DefaultNotifier&) = delete;
    DefaultNotifier(DefaultNotifier&&) = delete;
    DefaultNotifier& operator=(DefaultNotifier&&) = delete;

    [[nodiscard]] bool clear_all() noexcept override;

protected:
    std::shared_ptr<Subscription> subscribe_erased(
        std::type_index type,
        std::function<void(const void*)> callback) override;
    bool send_erased(std::type_index type, const void* event) override;

private:
    struct Slot;
    struct State;

    class SubscriptionImpl final : public Subscription {
    public:
        SubscriptionImpl(std::weak_ptr<State> state,
                         std::type_index type,
                         std::shared_ptr<Slot> slot);
        ~SubscriptionImpl() override;

        [[nodiscard]] bool unsubscribe() noexcept override;
        [[nodiscard]] bool active() const noexcept override;

    private:
        std::weak_ptr<State> state_;
        std::type_index type_;
        std::shared_ptr<Slot> slot_;
    };

    std::shared_ptr<State> state_;
};

}  // namespace semilive::publisher::infra
