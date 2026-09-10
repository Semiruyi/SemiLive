#include <semilive/publisher/contracts/output/video_access_unit_output_backend.hpp>
#include "publisher/support/output/scripted_video_output_backend.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace model = semilive::publisher::model;
namespace output = semilive::publisher::contracts::output;

using semilive::publisher::test_support::ScriptedVideoOutputBackend;
using semilive::publisher::test_support::ScriptedVideoOutputCallType;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

model::EncodedVideoAccessUnit access_unit() {
    return {
        {std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x65}},
        std::chrono::milliseconds{33},
        true,
        7,
        std::chrono::steady_clock::now(),
    };
}

void scripted_backend_preserves_contract_values_and_calls() {
    ScriptedVideoOutputBackend backend;
    backend.queue_open_result(output::VideoOutputInfo{"memory://video"});
    backend.queue_consume_result(output::VideoOutputReceipt{2, 1200});
    backend.queue_flush_result(output::VideoOutputReceipt{1, 48});
    const auto unit = access_unit();

    const auto opened = backend.open();
    const auto consumed = backend.consume(unit);
    const auto flushed = backend.flush();
    backend.close();

    require(opened && opened->output_name == "memory://video",
            "open must preserve output identity");
    require(consumed && consumed->emitted_units == 2 &&
                consumed->emitted_bytes == 1200,
            "consume must preserve its output receipt");
    require(flushed && flushed->emitted_units == 1 &&
                flushed->emitted_bytes == 48,
            "flush must preserve its output receipt");
    require(backend.consumed_access_units().size() == 1 &&
                backend.consumed_access_units().front().annex_b == unit.annex_b &&
                backend.consumed_access_units().front().source_sequence == 7,
            "consume must receive the complete access unit without mutation");

    const auto& calls = backend.calls();
    require(calls.size() == 4 &&
                calls[0].type == ScriptedVideoOutputCallType::Open &&
                calls[1].type == ScriptedVideoOutputCallType::Consume &&
                calls[2].type == ScriptedVideoOutputCallType::Flush &&
                calls[3].type == ScriptedVideoOutputCallType::Close,
            "backend calls must preserve invocation order");
    require(calls[1].source_sequence == 7,
            "consume call records must retain source metadata");
    for (const auto& call : calls) {
        require(call.thread_id == std::this_thread::get_id(),
                "backend calls must retain their invoking thread");
    }
}

void scripted_backend_preserves_structured_failures() {
    ScriptedVideoOutputBackend backend;
    backend.queue_consume_result(std::unexpected{output::VideoOutputIssue{
        output::VideoOutputOperation::Consume, -12, "output failed"}});

    const auto result = backend.consume(access_unit());
    require(!result &&
                result.error().operation == output::VideoOutputOperation::Consume &&
                result.error().native_code == -12 &&
                result.error().message == "output failed",
            "scripted failures must preserve structured issue details");
}

}  // namespace

int main() {
    try {
        scripted_backend_preserves_contract_values_and_calls();
        scripted_backend_preserves_structured_failures();
    } catch (const std::exception& error) {
        std::cerr << "publisher video output contract test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "publisher video output contract tests passed\n";
    return EXIT_SUCCESS;
}
