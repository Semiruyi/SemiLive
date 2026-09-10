#include <semilive/publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue.hpp>
#include <semilive/publisher/domain/worker/video_output_worker/default_video_output_worker.hpp>
#include <semilive/publisher/domain/worker/video_output_worker/video_output_worker_events.hpp>
#include "publisher/support/notifier/synchronous_notifier.hpp"
#include "publisher/support/output/scripted_video_output_backend.hpp"

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
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace output = semilive::publisher::contracts::output;
namespace model = semilive::publisher::model;
namespace publisher = semilive::publisher::domain;

using semilive::publisher::test_support::ScriptedVideoOutputBackend;
using semilive::publisher::test_support::ScriptedVideoOutputCallType;
using semilive::publisher::test_support::SynchronousNotifier;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename Predicate>
bool wait_until(Predicate predicate,
                const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

output::VideoOutputInfo output_info() {
    return {"scripted-video-output"};
}

output::VideoOutputReceipt receipt(const std::uint64_t units,
                                   const std::uint64_t bytes) {
    return {units, bytes};
}

model::EncodedVideoAccessUnit access_unit(const std::uint64_t sequence,
                                          const bool key_frame = false) {
    model::EncodedVideoAccessUnit result;
    result.annex_b = {std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
                      static_cast<std::byte>(sequence & 0xffU)};
    result.presentation_time = std::chrono::milliseconds{sequence * 33U};
    result.key_frame = key_frame;
    result.source_sequence = sequence;
    result.captured_at = std::chrono::steady_clock::now();
    return result;
}

void push(publisher::EncodedVideoAccessUnitQueue& queue,
          model::EncodedVideoAccessUnit unit) {
    require(queue.try_push(std::move(unit)) ==
                publisher::EncodedVideoAccessUnitPushResult::Accepted,
            "test AU must fit in the queue");
}

void backend_lifecycle_stays_on_the_worker_thread() {
    auto notifier = std::make_shared<SynchronousNotifier>();
    publisher::EncodedVideoAccessUnitQueue queue{notifier};
    auto backend = std::make_unique<ScriptedVideoOutputBackend>();
    auto* backend_view = backend.get();
    backend->queue_open_result(output_info());
    publisher::DefaultVideoOutputWorker worker{
        std::move(backend), queue, notifier};

    require(worker.state() == publisher::VideoOutputWorkerState::Idle,
            "constructed output worker must be idle");
    const auto controller_thread = std::this_thread::get_id();
    const auto started = worker.start();
    require(started && started->output.output_name == "scripted-video-output",
            "start must return actual output information");
    worker.stop(publisher::VideoOutputStopMode::Abort);

    const auto& calls = backend_view->calls();
    require(calls.size() == 2 &&
                calls[0].type == ScriptedVideoOutputCallType::Open &&
                calls[1].type == ScriptedVideoOutputCallType::Close,
            "abort must open and close without flushing");
    require(calls[0].thread_id == calls[1].thread_id &&
                calls[0].thread_id != controller_thread,
            "backend lifecycle must stay on one dedicated worker thread");
    require(worker.state() == publisher::VideoOutputWorkerState::Idle,
            "abort must restore idle state");
}

void not_empty_wakes_an_idle_worker_without_busy_polling() {
    auto notifier = std::make_shared<SynchronousNotifier>();
    publisher::EncodedVideoAccessUnitQueue queue{notifier};
    auto backend = std::make_unique<ScriptedVideoOutputBackend>();
    backend->queue_open_result(output_info());
    backend->queue_consume_result(receipt(1, 4));
    backend->queue_flush_result(receipt(0, 0));
    publisher::DefaultVideoOutputWorker worker{
        std::move(backend), queue, notifier};

    require(worker.start().has_value(), "output session must start");
    std::this_thread::sleep_for(25ms);
    require(worker.stats().backend_calls == 0,
            "an empty queue must not call or busy-poll the backend");

    push(queue, access_unit(1));
    require(wait_until([&worker] {
                return worker.stats().consumed_access_units == 1;
            }),
            "NotEmpty must wake the output worker");
    worker.stop(publisher::VideoOutputStopMode::Drain);
    require(worker.stats().flush_calls == 1,
            "normal drain must flush the backend exactly once");
}

void drain_preserves_order_and_aggregates_statistics() {
    auto worker_notifier = std::make_shared<SynchronousNotifier>();
    auto silent_queue_notifier = std::make_shared<SynchronousNotifier>();
    publisher::EncodedVideoAccessUnitQueue queue{silent_queue_notifier, 4};
    auto backend = std::make_unique<ScriptedVideoOutputBackend>();
    auto* backend_view = backend.get();
    backend->queue_open_result(output_info());
    backend->queue_consume_result(receipt(1, 4));
    backend->queue_consume_result(receipt(1, 5));
    backend->queue_consume_result(receipt(2, 12));
    backend->queue_flush_result(receipt(1, 3));
    publisher::DefaultVideoOutputWorker worker{
        std::move(backend), queue, worker_notifier};

    require(worker.start().has_value(), "output session must start");
    std::this_thread::sleep_for(25ms);
    push(queue, access_unit(2, true));
    auto middle = access_unit(5);
    middle.annex_b.push_back(std::byte{0x55});
    push(queue, std::move(middle));
    push(queue, access_unit(9));
    std::this_thread::sleep_for(10ms);
    require(queue.size() == 3,
            "silent queue must retain input until drain wakes the worker");

    worker.stop(publisher::VideoOutputStopMode::Drain);
    require(queue.empty(), "drain must consume every queued access unit");
    require(worker.state() == publisher::VideoOutputWorkerState::Idle,
            "successful drain must restore idle state");

    const auto& consumed = backend_view->consumed_access_units();
    require(consumed.size() == 3 &&
                consumed[0].source_sequence == 2 &&
                consumed[1].source_sequence == 5 &&
                consumed[2].source_sequence == 9,
            "drain must preserve access-unit order and metadata");
    const auto& calls = backend_view->calls();
    require(calls.size() == 6 &&
                calls[4].type == ScriptedVideoOutputCallType::Flush &&
                calls[5].type == ScriptedVideoOutputCallType::Close,
            "drain must flush once after all access units and then close");
    for (const auto& call : calls) {
        require(call.thread_id == calls.front().thread_id,
                "all backend calls must use the same output thread");
    }

    const auto stats = worker.stats();
    require(stats.output &&
                stats.output->output_name == "scripted-video-output",
            "worker stats must retain output identity");
    require(stats.consumed_access_units == 3 &&
                stats.drained_access_units == 3 &&
                stats.key_frames == 1 && stats.input_bytes == 13,
            "worker must aggregate input access-unit statistics");
    require(stats.backend_calls == 4 && stats.flush_calls == 1 &&
                stats.emitted_units == 5 && stats.emitted_bytes == 24,
            "worker must aggregate consume and flush receipts");
    require(stats.capture_to_output_samples == 3 &&
                stats.maximum_backend_time >= 0ns,
            "worker must record output timing samples");
}

void abort_leaves_queued_access_units_for_controller_cleanup() {
    auto worker_notifier = std::make_shared<SynchronousNotifier>();
    auto silent_queue_notifier = std::make_shared<SynchronousNotifier>();
    publisher::EncodedVideoAccessUnitQueue queue{silent_queue_notifier, 2};
    auto backend = std::make_unique<ScriptedVideoOutputBackend>();
    auto* backend_view = backend.get();
    backend->queue_open_result(output_info());
    publisher::DefaultVideoOutputWorker worker{
        std::move(backend), queue, worker_notifier};

    require(worker.start().has_value(), "output session must start");
    std::this_thread::sleep_for(25ms);
    push(queue, access_unit(1));
    push(queue, access_unit(2));
    const auto started_at = std::chrono::steady_clock::now();
    worker.stop(publisher::VideoOutputStopMode::Abort);

    require(std::chrono::steady_clock::now() - started_at < 500ms,
            "abort must not drain queued access units");
    require(queue.size() == 2,
            "queue cleanup must remain the controller's responsibility");
    const auto& calls = backend_view->calls();
    require(calls.size() == 2 &&
                calls[0].type == ScriptedVideoOutputCallType::Open &&
                calls[1].type == ScriptedVideoOutputCallType::Close,
            "abort must neither consume nor flush the backend");
}

void invalid_access_unit_fails_before_reaching_the_backend() {
    auto notifier = std::make_shared<SynchronousNotifier>();
    publisher::EncodedVideoAccessUnitQueue queue{notifier, 2};
    auto backend = std::make_unique<ScriptedVideoOutputBackend>();
    auto* backend_view = backend.get();
    backend->queue_open_result(output_info());
    backend->queue_consume_result(receipt(1, 4));
    publisher::DefaultVideoOutputWorker worker{
        std::move(backend), queue, notifier};

    require(worker.start().has_value(), "output session must start");
    push(queue, access_unit(3));
    require(wait_until([&worker] {
                return worker.stats().consumed_access_units == 1;
            }),
            "first valid access unit must reach the backend");

    auto invalid = access_unit(4);
    invalid.presentation_time = std::chrono::milliseconds{3U * 33U};
    push(queue, std::move(invalid));
    require(wait_until([&worker] {
                return worker.state() == publisher::VideoOutputWorkerState::Failed;
            }),
            "non-increasing presentation time must fail the output worker");
    require(backend_view->consumed_access_units().size() == 1,
            "invalid access unit must not reach the output backend");
    const auto stats = worker.stats();
    require(stats.fatal_failures == 1 && stats.last_issue &&
                stats.last_issue->operation ==
                    publisher::VideoOutputWorkerOperation::ValidateAccessUnit,
            "validation failure must retain its structured operation");
    worker.stop(publisher::VideoOutputStopMode::Abort);
}

void runtime_backend_failure_notifies_once_and_requires_stop() {
    auto notifier = std::make_shared<SynchronousNotifier>();
    publisher::EncodedVideoAccessUnitQueue queue{notifier};
    std::atomic_uint64_t failures{0};
    auto subscription = notifier->subscribe<publisher::VideoOutputWorkerFailed>(
        [&failures](const publisher::VideoOutputWorkerFailed& event) {
            if (event.issue.operation == publisher::VideoOutputWorkerOperation::Output) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    auto backend = std::make_unique<ScriptedVideoOutputBackend>();
    backend->queue_open_result(output_info());
    backend->queue_consume_result(std::unexpected{output::VideoOutputIssue{
        output::VideoOutputOperation::Consume, -7, "scripted output failed"}});
    publisher::DefaultVideoOutputWorker worker{
        std::move(backend), queue, notifier};

    require(worker.start().has_value(), "output session must start");
    push(queue, access_unit(1));
    require(wait_until([&worker] {
                return worker.state() == publisher::VideoOutputWorkerState::Failed;
            }),
            "runtime backend error must fail the worker");
    require(failures.load(std::memory_order_relaxed) == 1,
            "runtime backend error must notify exactly once");
    const auto stats = worker.stats();
    require(stats.fatal_failures == 1 && stats.last_issue &&
                stats.last_issue->output_issue &&
                stats.last_issue->output_issue->native_code == -7,
            "worker must preserve the structured backend issue");

    const auto restart = worker.start();
    require(!restart && restart.error().operation ==
                            publisher::VideoOutputWorkerOperation::Control,
            "failed worker must require stop before restart");
    worker.stop(publisher::VideoOutputStopMode::Abort);
    require(worker.state() == publisher::VideoOutputWorkerState::Idle,
            "abort must acknowledge and reset failed state");
    require(subscription->active(), "failure subscription must remain active");
}

void start_failure_returns_to_idle_and_allows_retry() {
    auto notifier = std::make_shared<SynchronousNotifier>();
    publisher::EncodedVideoAccessUnitQueue queue{notifier};
    auto backend = std::make_unique<ScriptedVideoOutputBackend>();
    auto* backend_view = backend.get();
    backend->queue_open_result(std::unexpected{output::VideoOutputIssue{
        output::VideoOutputOperation::Open, 12, "scripted open failed"}});
    backend->queue_open_result(output_info());
    backend->queue_flush_result(receipt(0, 0));
    publisher::DefaultVideoOutputWorker worker{
        std::move(backend), queue, notifier};

    const auto failed = worker.start();
    require(!failed && failed.error().output_issue &&
                failed.error().output_issue->native_code == 12,
            "start failure must preserve the backend issue");
    require(worker.state() == publisher::VideoOutputWorkerState::Idle,
            "start failure must restore idle state");
    require(worker.start().has_value(),
            "worker must retry after an output open failure");
    worker.stop(publisher::VideoOutputStopMode::Drain);

    const auto& calls = backend_view->calls();
    require(calls.size() == 5 &&
                calls[0].type == ScriptedVideoOutputCallType::Open &&
                calls[1].type == ScriptedVideoOutputCallType::Close &&
                calls[2].type == ScriptedVideoOutputCallType::Open &&
                calls[3].type == ScriptedVideoOutputCallType::Flush &&
                calls[4].type == ScriptedVideoOutputCallType::Close,
            "retry must use a fresh backend lifecycle");
}

void flush_failure_completes_drain_and_enters_failed_state() {
    auto notifier = std::make_shared<SynchronousNotifier>();
    publisher::EncodedVideoAccessUnitQueue queue{notifier};
    std::atomic_uint64_t failures{0};
    auto subscription = notifier->subscribe<publisher::VideoOutputWorkerFailed>(
        [&failures](const publisher::VideoOutputWorkerFailed& event) {
            if (event.issue.operation == publisher::VideoOutputWorkerOperation::Flush) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    auto backend = std::make_unique<ScriptedVideoOutputBackend>();
    backend->queue_open_result(output_info());
    backend->queue_flush_result(std::unexpected{output::VideoOutputIssue{
        output::VideoOutputOperation::Flush, 23, "scripted flush failed"}});
    publisher::DefaultVideoOutputWorker worker{
        std::move(backend), queue, notifier};

    require(worker.start().has_value(), "output session must start");
    worker.stop(publisher::VideoOutputStopMode::Drain);
    require(worker.state() == publisher::VideoOutputWorkerState::Failed,
            "flush failure during drain must enter failed state");
    require(failures.load(std::memory_order_relaxed) == 1,
            "flush failure must notify exactly once");
    const auto stats = worker.stats();
    require(stats.last_issue && stats.last_issue->output_issue &&
                stats.last_issue->output_issue->native_code == 23,
            "flush failure must preserve its backend issue");
    worker.stop(publisher::VideoOutputStopMode::Abort);
    require(subscription->active(), "failure subscription must remain active");
}

}  // namespace

int main() {
    try {
        backend_lifecycle_stays_on_the_worker_thread();
        not_empty_wakes_an_idle_worker_without_busy_polling();
        drain_preserves_order_and_aggregates_statistics();
        abort_leaves_queued_access_units_for_controller_cleanup();
        invalid_access_unit_fails_before_reaching_the_backend();
        runtime_backend_failure_notifies_once_and_requires_stop();
        start_failure_returns_to_idle_and_allows_retry();
        flush_failure_completes_drain_and_enters_failed_state();
    } catch (const std::exception& error) {
        std::cerr << "publisher video output worker test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher video output worker tests passed\n";
    return EXIT_SUCCESS;
}
