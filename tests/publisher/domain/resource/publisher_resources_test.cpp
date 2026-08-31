#include "publisher/domain/resource/captured_video_frame_store/captured_video_frame_store.hpp"
#include "publisher/domain/resource/captured_video_frame_store/captured_video_frame_store_events.hpp"
#include "publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue.hpp"
#include "publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue_events.hpp"
#include "publisher/infrastructure/notifier/default_notifier.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using semilive::publisher::domain::CapturedVideoFrame;
using semilive::publisher::domain::CapturedVideoFramePushResult;
using semilive::publisher::domain::CapturedVideoFrameStore;
using semilive::publisher::domain::CapturedVideoFrameStoreNotEmpty;
using semilive::publisher::domain::EncodedVideoAccessUnit;
using semilive::publisher::domain::EncodedVideoAccessUnitPushResult;
using semilive::publisher::domain::EncodedVideoAccessUnitQueue;
using semilive::publisher::domain::EncodedVideoAccessUnitQueueNotEmpty;
using semilive::publisher::domain::EncodedVideoAccessUnitQueueNotFull;
using semilive::publisher::infra::DefaultNotifier;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

CapturedVideoFrame frame(const std::uint64_t sequence) {
    CapturedVideoFrame result;
    result.bgra.resize(4);
    result.width = 1;
    result.height = 1;
    result.stride = 4;
    result.sequence = sequence;
    result.captured_at = std::chrono::steady_clock::now();
    return result;
}

EncodedVideoAccessUnit access_unit(const std::uint64_t sequence) {
    EncodedVideoAccessUnit result;
    result.annex_b.resize(8, std::byte{0x01});
    result.pts_90khz = static_cast<std::int64_t>(sequence * 3'000);
    result.source_sequence = sequence;
    result.captured_at = std::chrono::steady_clock::now();
    return result;
}

void captured_video_frame_store_replaces_oldest() {
    auto notifier = std::make_shared<DefaultNotifier>();
    CapturedVideoFrameStore store{notifier, 2};
    auto first = frame(1);
    auto second = frame(2);
    auto third = frame(3);

    require(store.try_push(std::move(first)) == CapturedVideoFramePushResult::Accepted,
            "first frame must be accepted");
    require(store.try_push(std::move(second)) == CapturedVideoFramePushResult::Accepted,
            "second frame must be accepted");
    require(store.try_push(std::move(third)) == CapturedVideoFramePushResult::ReplacedOldest,
            "third frame must replace the oldest frame");

    const auto popped_second = store.try_pop();
    const auto popped_third = store.try_pop();
    require(popped_second && popped_second->sequence == 2,
            "replacement must discard sequence 1");
    require(popped_third && popped_third->sequence == 3,
            "newest frame must remain available");
    require(store.empty(), "store must be empty after both frames are consumed");
    require(store.peak_size() == 2, "frame store peak size must equal capacity");
    require(store.replaced_count() == 1, "frame replacement must be counted");
}

void captured_video_frame_store_notifies_empty_to_non_empty_and_clears() {
    auto notifier = std::make_shared<DefaultNotifier>();
    CapturedVideoFrameStore store{notifier, 2};
    int not_empty_count = 0;
    auto subscription = notifier->subscribe<CapturedVideoFrameStoreNotEmpty>(
        [&not_empty_count](const CapturedVideoFrameStoreNotEmpty&) {
            ++not_empty_count;
        });

    auto first = frame(1);
    auto second = frame(2);
    require(store.try_push(std::move(first)) == CapturedVideoFramePushResult::Accepted,
            "first frame must be accepted");
    require(store.try_push(std::move(second)) == CapturedVideoFramePushResult::Accepted,
            "second frame must be accepted");
    require(not_empty_count == 1, "non-empty notification must be edge-triggered");

    require(store.clear() == 2, "clear must report both discarded frames");
    require(store.empty(), "clear must empty the frame store");

    auto third = frame(3);
    require(store.try_push(std::move(third)) == CapturedVideoFramePushResult::Accepted,
            "frame after clear must be accepted");
    require(not_empty_count == 2, "push after clear must publish a new edge");
    require(subscription->active(), "resource notification subscription must remain active");
}

void encoded_video_access_unit_queue_preserves_pending_item_and_notifies_edges() {
    auto notifier = std::make_shared<DefaultNotifier>();
    EncodedVideoAccessUnitQueue queue{notifier, 2};
    int not_empty_count = 0;
    int not_full_count = 0;
    auto not_empty_subscription = notifier->subscribe<EncodedVideoAccessUnitQueueNotEmpty>(
        [&not_empty_count](const EncodedVideoAccessUnitQueueNotEmpty&) {
            ++not_empty_count;
        });
    auto not_full_subscription = notifier->subscribe<EncodedVideoAccessUnitQueueNotFull>(
        [&not_full_count](const EncodedVideoAccessUnitQueueNotFull&) {
            ++not_full_count;
        });

    auto first = access_unit(1);
    auto second = access_unit(2);
    auto pending = access_unit(3);
    require(queue.try_push(std::move(first)) == EncodedVideoAccessUnitPushResult::Accepted,
            "first AU must be accepted");
    require(queue.try_push(std::move(second)) == EncodedVideoAccessUnitPushResult::Accepted,
            "second AU must be accepted");
    require(not_empty_count == 1, "AU not-empty notification must be edge-triggered");
    require(queue.full(), "two AUs must fill a capacity-two queue");

    require(queue.try_push(std::move(pending)) == EncodedVideoAccessUnitPushResult::Full,
            "full AU queue must reject the pending AU");
    require(pending.source_sequence == 3 && !pending.annex_b.empty(),
            "full result must not consume the pending AU");

    const auto popped_first = queue.try_pop();
    require(popped_first && popped_first->source_sequence == 1,
            "AU queue must preserve FIFO order");
    require(not_full_count == 1, "full-to-non-full transition must notify the producer");
    require(queue.try_push(std::move(pending)) == EncodedVideoAccessUnitPushResult::Accepted,
            "pending AU must be accepted after space becomes available");

    const auto popped_second = queue.try_pop();
    const auto popped_third = queue.try_pop();
    require(popped_second && popped_second->source_sequence == 2,
            "second AU must remain in order");
    require(popped_third && popped_third->source_sequence == 3,
            "retried pending AU must remain in order");
    require(not_full_count == 2, "each full-to-non-full transition must notify once");
    require(queue.empty(), "AU queue must be empty after all items are consumed");
    require(queue.peak_size() == 2, "AU queue peak size must equal capacity");
    require(not_empty_subscription->active() && not_full_subscription->active(),
            "AU subscriptions must remain active");
}

void encoded_video_access_unit_queue_clear_reports_discard_and_releases_full_edge() {
    auto notifier = std::make_shared<DefaultNotifier>();
    EncodedVideoAccessUnitQueue queue{notifier, 2};
    int not_full_count = 0;
    auto subscription = notifier->subscribe<EncodedVideoAccessUnitQueueNotFull>(
        [&not_full_count](const EncodedVideoAccessUnitQueueNotFull&) {
            ++not_full_count;
        });

    auto first = access_unit(1);
    auto second = access_unit(2);
    require(queue.try_push(std::move(first)) == EncodedVideoAccessUnitPushResult::Accepted,
            "first AU must be accepted");
    require(queue.try_push(std::move(second)) == EncodedVideoAccessUnitPushResult::Accepted,
            "second AU must be accepted");

    require(queue.clear() == 2, "clear must report both discarded AUs");
    require(queue.empty(), "clear must empty the AU queue");
    require(not_full_count == 1, "clearing a full queue must publish NotFull");
    require(subscription->active(), "not-full subscription must remain active");
}

void notifier_subscription_lifetime_controls_delivery() {
    DefaultNotifier notifier;
    int calls = 0;
    auto subscription = notifier.subscribe<CapturedVideoFrameStoreNotEmpty>(
        [&calls](const CapturedVideoFrameStoreNotEmpty&) {
            ++calls;
        });

    require(notifier.send(CapturedVideoFrameStoreNotEmpty{}),
            "event with an active subscriber must be dispatched");
    require(calls == 1, "subscriber must receive the matching event");
    require(subscription->unsubscribe(), "first unsubscribe must succeed");
    require(!subscription->unsubscribe(), "unsubscribe must be idempotent");
    require(!notifier.send(CapturedVideoFrameStoreNotEmpty{}),
            "event without subscribers must not be dispatched");
    require(calls == 1, "unsubscribed callback must not run again");
}

void notifier_callback_failure_does_not_stop_delivery() {
    DefaultNotifier notifier;
    int calls = 0;

    auto throwing_subscription = notifier.subscribe<CapturedVideoFrameStoreNotEmpty>(
        [](const CapturedVideoFrameStoreNotEmpty&) { throw std::runtime_error{"expected failure"}; });
    auto healthy_subscription = notifier.subscribe<CapturedVideoFrameStoreNotEmpty>(
        [&calls](const CapturedVideoFrameStoreNotEmpty&) { ++calls; });

    require(notifier.send(CapturedVideoFrameStoreNotEmpty{}),
            "event with a throwing callback must still be dispatched");
    require(calls == 1, "a throwing callback must not stop later callbacks");
    require(throwing_subscription->active() && healthy_subscription->active(),
            "callback failure must not alter subscription lifetime");
}

void encoded_video_access_unit_queue_supports_concurrent_spsc_access() {
    auto notifier = std::make_shared<DefaultNotifier>();
    EncodedVideoAccessUnitQueue queue{notifier, 8};
    constexpr std::uint64_t kItemCount = 10'000;
    std::atomic_bool order_is_valid{true};

    std::jthread producer{[&] {
        for (std::uint64_t sequence = 1; sequence <= kItemCount; ++sequence) {
            auto item = access_unit(sequence);
            while (queue.try_push(std::move(item)) == EncodedVideoAccessUnitPushResult::Full) {
                std::this_thread::yield();
            }
        }
    }};

    std::jthread consumer{[&] {
        std::uint64_t expected = 1;
        while (expected <= kItemCount) {
            auto item = queue.try_pop();
            if (!item) {
                std::this_thread::yield();
                continue;
            }
            if (item->source_sequence != expected) {
                order_is_valid.store(false, std::memory_order_relaxed);
            }
            ++expected;
        }
    }};

    producer.join();
    consumer.join();
    require(order_is_valid.load(std::memory_order_relaxed),
            "concurrent SPSC access must preserve AU order");
    require(queue.empty(), "concurrent consumer must drain the AU queue");
}

void zero_capacity_is_rejected() {
    auto notifier = std::make_shared<DefaultNotifier>();
    bool frame_store_rejected = false;
    try {
        [[maybe_unused]] CapturedVideoFrameStore store{notifier, 0};
    } catch (const std::invalid_argument&) {
        frame_store_rejected = true;
    }
    require(frame_store_rejected, "zero-capacity frame store must be rejected");

    bool access_unit_queue_rejected = false;
    try {
        [[maybe_unused]] EncodedVideoAccessUnitQueue queue{notifier, 0};
    } catch (const std::invalid_argument&) {
        access_unit_queue_rejected = true;
    }
    require(access_unit_queue_rejected, "zero-capacity AU queue must be rejected");
}

}  // namespace

int main() {
    try {
        captured_video_frame_store_replaces_oldest();
        captured_video_frame_store_notifies_empty_to_non_empty_and_clears();
        encoded_video_access_unit_queue_preserves_pending_item_and_notifies_edges();
        encoded_video_access_unit_queue_clear_reports_discard_and_releases_full_edge();
        notifier_subscription_lifetime_controls_delivery();
        notifier_callback_failure_does_not_stop_delivery();
        encoded_video_access_unit_queue_supports_concurrent_spsc_access();
        zero_capacity_is_rejected();
    } catch (const std::exception& error) {
        std::cerr << "publisher resource test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher resource tests passed\n";
    return EXIT_SUCCESS;
}
