#include "publisher/contracts/encoder/video_encoder_backend.hpp"
#include "publisher/support/encoder/scripted_video_encoder_backend.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

namespace encoder = semilive::publisher::contracts::encoder;
namespace model = semilive::publisher::model;
using semilive::publisher::test_support::ScriptedVideoEncoderBackend;
using semilive::publisher::test_support::ScriptedVideoEncoderCallType;
using namespace std::chrono_literals;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

model::CapturedVideoFrame frame(const std::uint64_t sequence) {
    model::CapturedVideoFrame result;
    result.sequence = sequence;
    return result;
}

encoder::VideoEncodeBatch batch(
    const std::initializer_list<std::uint64_t> source_sequences) {
    encoder::VideoEncodeBatch result;
    result.preprocessing_time = 1ms;
    result.codec_time = 2ms;
    for (const auto sequence : source_sequences) {
        model::EncodedVideoAccessUnit access_unit;
        access_unit.source_sequence = sequence;
        result.access_units.push_back(std::move(access_unit));
    }
    return result;
}

void default_config_matches_the_first_video_milestone() {
    const encoder::VideoEncoderConfig config;
    require(config.output == model::VideoDimensions{1920, 1080},
            "default output must be 1920x1080");
    require(config.frame_rate == model::FrameRate{30, 1},
            "default frame rate must be 30 fps");
    require(config.target_bit_rate == 4'000'000,
            "default target bit rate must be 4 Mbps");
    require(config.gop_size == 60, "default GOP must contain 60 frames");
}

void scripted_backend_supports_delayed_and_multiple_output() {
    ScriptedVideoEncoderBackend backend;
    const encoder::VideoEncoderInfo info{
        {1920, 1080}, {30, 1}, 4'000'000, 60, 0, "scripted"};
    backend.queue_open_result(info);
    backend.queue_encode_result(batch({}));
    backend.queue_encode_result(batch({1}));
    backend.queue_encode_result(batch({2, 3}));
    backend.queue_flush_result(batch({4}));

    const encoder::VideoEncoderConfig config;
    const auto opened = backend.open(config);
    const auto first = backend.encode(frame(1));
    const auto second = backend.encode(frame(2));
    const auto third = backend.encode(frame(3));
    const auto flushed = backend.flush();
    backend.close();

    require(opened && opened->encoder_name == "scripted",
            "scripted open result must be returned");
    require(first && first->access_units.empty(),
            "an input may produce no immediate access unit");
    require(second && second->access_units.size() == 1 &&
                second->access_units.front().source_sequence == 1,
            "a later input may release a delayed access unit");
    require(third && third->access_units.size() == 2,
            "one input may produce multiple access units");
    require(flushed && flushed->access_units.size() == 1 &&
                flushed->access_units.front().source_sequence == 4,
            "flush may release retained access units");
}

void scripted_backend_records_calls_for_worker_tests() {
    ScriptedVideoEncoderBackend backend;
    backend.queue_open_result(encoder::VideoEncoderInfo{});
    backend.queue_encode_result(batch({}));
    backend.queue_flush_result(batch({}));

    (void)backend.open({});
    (void)backend.encode(frame(42));
    (void)backend.flush();
    backend.close();

    const auto& calls = backend.calls();
    require(calls.size() == 4, "every backend call must be recorded");
    require(calls[0].type == ScriptedVideoEncoderCallType::Open &&
                calls[1].type == ScriptedVideoEncoderCallType::Encode &&
                calls[2].type == ScriptedVideoEncoderCallType::Flush &&
                calls[3].type == ScriptedVideoEncoderCallType::Close,
            "backend calls must preserve invocation order");
    require(calls[1].source_sequence == 42,
            "encode calls must retain the input source sequence");
    for (const auto& call : calls) {
        require(call.thread_id == std::this_thread::get_id(),
                "backend calls must retain their invoking thread");
    }
    require(backend.open_configs().size() == 1 &&
                backend.encoded_frames().size() == 1,
            "backend inputs must remain available for assertions");
}

void scripted_backend_injects_structured_failures() {
    ScriptedVideoEncoderBackend backend;
    const encoder::VideoEncoderIssue failure{
        encoder::VideoEncoderOperation::ConvertFrame, -12, "conversion failed"};
    backend.queue_encode_result(std::unexpected{failure});

    const auto result = backend.encode(frame(7));
    require(!result &&
                result.error().operation == encoder::VideoEncoderOperation::ConvertFrame &&
                result.error().native_code == -12 &&
                result.error().message == "conversion failed",
            "scripted failures must preserve structured issue details");
}

}  // namespace

int main() {
    try {
        default_config_matches_the_first_video_milestone();
        scripted_backend_supports_delayed_and_multiple_output();
        scripted_backend_records_calls_for_worker_tests();
        scripted_backend_injects_structured_failures();
    } catch (const std::exception& error) {
        std::cerr << "publisher video encoder contract tests failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher video encoder contract tests passed\n";
    return EXIT_SUCCESS;
}
