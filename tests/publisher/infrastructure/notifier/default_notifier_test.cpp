#include <semilive/publisher/infrastructure/notifier/default_notifier.hpp>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using semilive::publisher::infra::DefaultNotifier;

struct TestEvent {
    std::uint32_t value = 0;
};

struct OtherEvent {};

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

void subscription_lifetime_controls_delivery() {
    DefaultNotifier notifier;
    int calls = 0;
    auto subscription = notifier.subscribe<TestEvent>(
        [&calls](const TestEvent&) { ++calls; });

    require(notifier.send(TestEvent{}),
            "event with an active subscriber must be dispatched");
    require(calls == 1, "subscriber must receive the matching event");
    require(subscription->unsubscribe(), "first unsubscribe must succeed");
    require(!subscription->unsubscribe(), "unsubscribe must be idempotent");
    require(!notifier.send(TestEvent{}),
            "event without subscribers must not be dispatched");
    require(calls == 1, "unsubscribed callback must not run again");
}

void event_types_are_isolated() {
    DefaultNotifier notifier;
    int calls = 0;
    auto subscription = notifier.subscribe<TestEvent>(
        [&calls](const TestEvent&) { ++calls; });

    require(!notifier.send(OtherEvent{}),
            "an unrelated event type must not be dispatched");
    require(calls == 0, "an unrelated event type must not invoke the callback");
    require(subscription->active(), "an unrelated event must not alter the subscription");
}

void callback_failure_does_not_stop_delivery() {
    DefaultNotifier notifier;
    int calls = 0;

    auto throwing_subscription = notifier.subscribe<TestEvent>(
        [](const TestEvent&) { throw std::runtime_error{"expected failure"}; });
    auto healthy_subscription = notifier.subscribe<TestEvent>(
        [&calls](const TestEvent&) { ++calls; });

    require(notifier.send(TestEvent{}),
            "event with a throwing callback must still be dispatched");
    require(calls == 1, "a throwing callback must not stop later callbacks");
    require(throwing_subscription->active() && healthy_subscription->active(),
            "callback failure must not alter subscription lifetime");
}

void concurrent_senders_are_supported() {
    DefaultNotifier notifier;
    std::atomic_uint64_t calls{0};
    std::atomic_bool dispatch_failed{false};
    auto subscription = notifier.subscribe<TestEvent>(
        [&calls](const TestEvent&) {
            calls.fetch_add(1, std::memory_order_relaxed);
        });

    constexpr std::uint64_t kThreadCount = 4;
    constexpr std::uint64_t kEventsPerThread = 1'000;
    std::vector<std::jthread> senders;
    senders.reserve(kThreadCount);
    for (std::uint64_t thread = 0; thread < kThreadCount; ++thread) {
        senders.emplace_back([&notifier, &dispatch_failed] {
            for (std::uint64_t index = 0; index < kEventsPerThread; ++index) {
                if (!notifier.send(TestEvent{})) {
                    dispatch_failed.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    senders.clear();

    require(!dispatch_failed.load(std::memory_order_relaxed),
            "every concurrent event must be dispatched");
    require(calls.load(std::memory_order_relaxed) == kThreadCount * kEventsPerThread,
            "concurrent senders must deliver every event");
    require(subscription->active(), "concurrent sends must not alter the subscription");
}

}  // namespace

int main() {
    try {
        subscription_lifetime_controls_delivery();
        event_types_are_isolated();
        callback_failure_does_not_stop_delivery();
        concurrent_senders_are_supported();
    } catch (const std::exception& error) {
        std::cerr << "default notifier test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "default notifier tests passed\n";
    return EXIT_SUCCESS;
}
