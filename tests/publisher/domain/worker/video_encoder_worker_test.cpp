#include <semilive/publisher/domain/resource/captured_video_frame_store/captured_video_frame_store.hpp>
#include <semilive/publisher/domain/resource/encoded_video_access_unit_queue/encoded_video_access_unit_queue.hpp>
#include <semilive/publisher/domain/worker/video_encoder_worker/default_video_encoder_worker.hpp>
#include <semilive/publisher/domain/worker/video_encoder_worker/video_encoder_worker_events.hpp>
#include "publisher/support/encoder/scripted_video_encoder_backend.hpp"
#include "publisher/support/notifier/synchronous_notifier.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
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

namespace encoder = semilive::publisher::contracts::encoder;
namespace model = semilive::publisher::model;
namespace publisher = semilive::publisher::domain;

using semilive::publisher::test_support::ScriptedVideoEncoderBackend;
using semilive::publisher::test_support::ScriptedVideoEncoderCallType;
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

encoder::VideoEncoderInfo encoder_info() {
    return {{1920, 1080}, {30, 1}, 4'000'000, 60, 1, "scripted"};
}

model::CapturedVideoFrame frame(const std::uint64_t sequence) {
    model::CapturedVideoFrame result;
    result.sequence = sequence;
    result.presentation_time = std::chrono::milliseconds{sequence * 33U};
    result.captured_at = std::chrono::steady_clock::now();
    return result;
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

encoder::VideoEncodeBatch batch(
    std::initializer_list<model::EncodedVideoAccessUnit> access_units,
    const std::chrono::nanoseconds preprocessing_time = 1ms,
    const std::chrono::nanoseconds codec_time = 2ms) {
    encoder::VideoEncodeBatch result;
    result.preprocessing_time = preprocessing_time;
    result.codec_time = codec_time;
    for (const auto& unit : access_units) {
        result.access_units.push_back(unit);
    }
    return result;
}

void backend_lifecycle_stays_on_the_worker_thread() {
    auto notifier = std::make_shared<SynchronousNotifier>();
    publisher::CapturedVideoFrameStore frames{notifier};
    publisher::EncodedVideoAccessUnitQueue access_units{notifier};
    auto backend = std::make_unique<ScriptedVideoEncoderBackend>();
    auto* backend_view = backend.get();
    backend->queue_open_result(encoder_info());
    publisher::DefaultVideoEncoderWorker worker{
        std::move(backend), frames, access_units, notifier};

    require(worker.state() == publisher::VideoEncoderWorkerState::Idle,
            "constructed encoder worker must be idle");
    const auto controller_thread = std::this_thread::get_id();
    const auto started = worker.start({});
    require(started && started->encoder.encoder_name == "scripted",
            "start must return actual encoder information");
    worker.stop(publisher::VideoEncoderStopMode::Abort);

    const auto& calls = backend_view->calls();
    require(calls.size() == 2 &&
                calls[0].type == ScriptedVideoEncoderCallType::Open &&
                calls[1].type == ScriptedVideoEncoderCallType::Close,
            "abort must open and close without flushing");
    require(calls[0].thread_id == calls[1].thread_id &&
                calls[0].thread_id != controller_thread,
            "backend lifecycle must stay on one dedicated worker thread");
    require(worker.state() == publisher::VideoEncoderWorkerState::Idle,
            "abort must restore idle state");
}

void drain_preserves_batches_order_and_statistics() {
    auto notifier = std::make_shared<SynchronousNotifier>();
    publisher::CapturedVideoFrameStore frames{notifier, 4};
    publisher::EncodedVideoAccessUnitQueue access_units{notifier, 8};
    auto backend = std::make_unique<ScriptedVideoEncoderBackend>();
    auto* backend_view = backend.get();
    backend->queue_open_result(encoder_info());
    backend->queue_encode_result(batch({}));
    backend->queue_encode_result(
        batch({access_unit(2, true), access_unit(5)}));
    backend->queue_flush_result(batch({access_unit(6)}));
    publisher::DefaultVideoEncoderWorker worker{
        std::move(backend), frames, access_units, notifier};

    require(worker.start({}).has_value(), "encoder session must start");
    (void)frames.try_push(frame(2));
    (void)frames.try_push(frame(5));
    require(wait_until([&worker] {
                return worker.stats().consumed_frames == 2;
            }),
            "worker must consume all available input frames");
    worker.stop(publisher::VideoEncoderStopMode::Drain);

    std::vector<std::uint64_t> sequences;
    while (auto unit = access_units.try_pop()) {
        sequences.push_back(unit->source_sequence);
    }
    require(sequences == std::vector<std::uint64_t>{2, 5, 6},
            "encode and flush access units must preserve order");

    const auto stats = worker.stats();
    require(stats.zero_output_calls == 1 &&
                stats.multiple_output_calls == 1,
            "worker must classify zero and multiple output calls");
    require(stats.input_sequence_gaps == 1 &&
                stats.missing_input_frames == 2,
            "worker must record source sequence gaps without rewriting them");
    require(stats.produced_access_units == 3 &&
                stats.submitted_access_units == 3 &&
                stats.flushed_access_units == 1 &&
                stats.key_frames == 1 && stats.encoded_bytes == 12,
            "worker must aggregate access unit statistics");
    require(stats.backend_calls == 3 &&
                stats.total_preprocessing_time == 3ms &&
                stats.total_codec_time == 6ms,
            "worker must aggregate backend timing diagnostics");
    require(worker.state() == publisher::VideoEncoderWorkerState::Idle,
            "successful drain must restore idle state");

    const auto& calls = backend_view->calls();
    require(calls.size() == 5 &&
                calls[3].type == ScriptedVideoEncoderCallType::Flush &&
                calls[4].type == ScriptedVideoEncoderCallType::Close,
            "drain must flush exactly once before closing");
}

void a_full_access_unit_queue_stops_input_consumption() {
    auto notifier = std::make_shared<SynchronousNotifier>();
    publisher::CapturedVideoFrameStore frames{notifier, 4};
    publisher::EncodedVideoAccessUnitQueue access_units{notifier, 1};
    auto backend = std::make_unique<ScriptedVideoEncoderBackend>();
    backend->queue_open_result(encoder_info());
    backend->queue_encode_result(
        batch({access_unit(1), access_unit(11)}));
    backend->queue_encode_result(batch({access_unit(2)}));
    backend->queue_flush_result(batch({}));
    publisher::DefaultVideoEncoderWorker worker{
        std::move(backend), frames, access_units, notifier};

    require(worker.start({}).has_value(), "encoder session must start");
    (void)frames.try_push(frame(1));
    (void)frames.try_push(frame(2));
    require(wait_until([&] {
                return access_units.size() == 1 &&
                       worker.stats().consumed_frames == 1 &&
                       worker.stats().pending_access_units == 1;
            }),
            "multi-output batch must retain one pending access unit");
    std::this_thread::sleep_for(25ms);
    require(worker.stats().consumed_frames == 1,
            "worker must not consume another frame while output is full");

    auto first = access_units.try_pop();
    require(first && first->source_sequence == 1,
            "first access unit must remain available to the sender");
    require(wait_until([&] {
                return access_units.size() == 1 &&
                       worker.stats().pending_access_units == 0;
            }),
            "NotFull must submit the retained pending access unit first");
    require(worker.stats().consumed_frames == 1,
            "worker must keep input paused while pending output refills the queue");
    auto second = access_units.try_pop();
    require(second && second->source_sequence == 11,
            "pending output must be ordered before consuming another frame");
    require(wait_until([&] {
                return access_units.size() == 1 &&
                       worker.stats().consumed_frames == 2;
            }),
            "worker must resume input only after pending output is drained");
    auto third = access_units.try_pop();
    require(third && third->source_sequence == 2,
            "resumed worker must preserve the next frame's access unit");

    worker.stop(publisher::VideoEncoderStopMode::Drain);
    const auto stats = worker.stats();
    require(stats.access_unit_queue_full_events >= 1 &&
                stats.total_backpressure_time > 0ns,
            "worker must record bounded output backpressure");
}

void drain_waits_only_until_all_pending_output_is_accepted() {
    auto notifier = std::make_shared<SynchronousNotifier>();
    publisher::CapturedVideoFrameStore frames{notifier};
    publisher::EncodedVideoAccessUnitQueue access_units{notifier, 1};
    auto backend = std::make_unique<ScriptedVideoEncoderBackend>();
    backend->queue_open_result(encoder_info());
    backend->queue_encode_result(
        batch({access_unit(1), access_unit(11)}));
    backend->queue_flush_result(batch({access_unit(12)}));
    publisher::DefaultVideoEncoderWorker worker{
        std::move(backend), frames, access_units, notifier};

    require(worker.start({}).has_value(), "encoder session must start");
    (void)frames.try_push(frame(1));
    require(wait_until([&] {
                return access_units.size() == 1 &&
                       worker.stats().pending_access_units == 1;
            }),
            "test must establish one queued and one pending access unit");

    auto stopped = std::async(std::launch::async, [&worker] {
        worker.stop(publisher::VideoEncoderStopMode::Drain);
    });
    require(wait_until([&worker] {
                return worker.state() ==
                       publisher::VideoEncoderWorkerState::Draining;
            }),
            "drain command must enter draining state");
    require(stopped.wait_for(25ms) == std::future_status::timeout,
            "drain must wait while a pending access unit is backpressured");

    auto first = access_units.try_pop();
    require(first && first->source_sequence == 1,
            "drain must preserve the first encoded access unit");
    require(wait_until([&] {
                return access_units.size() == 1 &&
                       worker.stats().pending_access_units == 0;
            }),
            "drain must submit pending output before flushing");
    require(stopped.wait_for(25ms) == std::future_status::timeout,
            "drain must wait for capacity before producing flush output");

    auto second = access_units.try_pop();
    require(second && second->source_sequence == 11,
            "drain must preserve pending output order");
    require(stopped.wait_for(2s) == std::future_status::ready,
            "drain must finish once its flush output is accepted");
    stopped.get();
    auto flushed = access_units.try_pop();
    require(flushed && flushed->source_sequence == 12,
            "drain must leave accepted flush output for the sender");
    require(worker.state() == publisher::VideoEncoderWorkerState::Idle,
            "a full queue containing only accepted output must not delay drain");
}

void abort_is_bounded_when_the_output_queue_is_full() {
    auto notifier = std::make_shared<SynchronousNotifier>();
    publisher::CapturedVideoFrameStore frames{notifier};
    publisher::EncodedVideoAccessUnitQueue access_units{notifier, 1};
    auto backend = std::make_unique<ScriptedVideoEncoderBackend>();
    auto* backend_view = backend.get();
    backend->queue_open_result(encoder_info());
    publisher::DefaultVideoEncoderWorker worker{
        std::move(backend), frames, access_units, notifier};

    require(worker.start({}).has_value(), "encoder session must start");
    require(access_units.try_push(access_unit(90)) ==
                publisher::EncodedVideoAccessUnitPushResult::Accepted,
            "test must prefill the output queue");
    (void)frames.try_push(frame(1));
    require(wait_until([&worker] {
                return worker.stats().access_unit_queue_full_events >= 1;
            }),
            "worker must observe output backpressure");

    const auto started_at = std::chrono::steady_clock::now();
    worker.stop(publisher::VideoEncoderStopMode::Abort);
    require(std::chrono::steady_clock::now() - started_at < 500ms,
            "abort must not wait for output queue capacity");
    const auto& calls = backend_view->calls();
    require(calls.size() == 2 &&
                calls[0].type == ScriptedVideoEncoderCallType::Open &&
                calls[1].type == ScriptedVideoEncoderCallType::Close,
            "abort under backpressure must neither encode nor flush");
}

void runtime_failure_notifies_once_and_requires_stop() {
    auto notifier = std::make_shared<SynchronousNotifier>();
    publisher::CapturedVideoFrameStore frames{notifier};
    publisher::EncodedVideoAccessUnitQueue access_units{notifier};
    std::atomic_uint64_t failures{0};
    auto subscription = notifier->subscribe<publisher::VideoEncoderWorkerFailed>(
        [&failures](const publisher::VideoEncoderWorkerFailed& event) {
            if (event.issue.operation ==
                publisher::VideoEncoderWorkerOperation::Encode) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    auto backend = std::make_unique<ScriptedVideoEncoderBackend>();
    backend->queue_open_result(encoder_info());
    backend->queue_encode_result(std::unexpected{encoder::VideoEncoderIssue{
        encoder::VideoEncoderOperation::ConvertFrame, -7, "convert failed"}});
    publisher::DefaultVideoEncoderWorker worker{
        std::move(backend), frames, access_units, notifier};

    require(worker.start({}).has_value(), "encoder session must start");
    (void)frames.try_push(frame(1));
    require(wait_until([&worker] {
                return worker.state() == publisher::VideoEncoderWorkerState::Failed;
            }),
            "runtime backend error must fail the worker");
    require(failures.load(std::memory_order_relaxed) == 1,
            "runtime backend error must notify exactly once");
    const auto stats = worker.stats();
    require(stats.fatal_failures == 1 && stats.last_issue &&
                stats.last_issue->encoder_issue &&
                stats.last_issue->encoder_issue->native_code == -7,
            "worker must preserve the structured backend issue");

    const auto restart = worker.start({});
    require(!restart && restart.error().operation ==
                            publisher::VideoEncoderWorkerOperation::Control,
            "failed worker must require stop before restart");
    worker.stop(publisher::VideoEncoderStopMode::Abort);
    require(worker.state() == publisher::VideoEncoderWorkerState::Idle,
            "abort must acknowledge and reset failed state");
    require(subscription->active(), "failure subscription must remain active");
}

}  // namespace

int main() {
    try {
        backend_lifecycle_stays_on_the_worker_thread();
        drain_preserves_batches_order_and_statistics();
        a_full_access_unit_queue_stops_input_consumption();
        drain_waits_only_until_all_pending_output_is_accepted();
        abort_is_bounded_when_the_output_queue_is_full();
        runtime_failure_notifies_once_and_requires_stop();
    } catch (const std::exception& error) {
        std::cerr << "publisher video encoder worker test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher video encoder worker tests passed\n";
    return EXIT_SUCCESS;
}
