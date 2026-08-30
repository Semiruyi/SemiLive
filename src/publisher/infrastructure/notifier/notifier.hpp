#pragma once

#include <functional>
#include <memory>
#include <typeindex>
#include <typeinfo>
#include <utility>

namespace semilive::publisher::infra {

// Type-based synchronous notification contract. Callbacks run on the sender's
// thread and therefore must only publish lightweight wake-up hints.
class Notifier {
public:
    class Subscription {
    public:
        virtual ~Subscription() = default;

        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&&) = delete;
        Subscription& operator=(Subscription&&) = delete;

        [[nodiscard]] virtual bool unsubscribe() noexcept = 0;
        [[nodiscard]] virtual bool active() const noexcept = 0;

    protected:
        Subscription() = default;
    };

    virtual ~Notifier() = default;

    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;
    Notifier(Notifier&&) = delete;
    Notifier& operator=(Notifier&&) = delete;

    template <class Event>
    [[nodiscard]] std::shared_ptr<Subscription> subscribe(
        std::function<void(const Event&)> callback) {
        return subscribe_erased(
            std::type_index(typeid(Event)),
            [callback = std::move(callback)](const void* event) {
                callback(*static_cast<const Event*>(event));
            });
    }

    template <class Event>
    [[nodiscard]] bool send(const Event& event) {
        return send_erased(std::type_index(typeid(Event)), &event);
    }

    [[nodiscard]] virtual bool clear_all() noexcept = 0;

protected:
    Notifier() = default;

    virtual std::shared_ptr<Subscription> subscribe_erased(
        std::type_index type,
        std::function<void(const void*)> callback) = 0;
    virtual bool send_erased(std::type_index type, const void* event) = 0;
};

}  // namespace semilive::publisher::infra
