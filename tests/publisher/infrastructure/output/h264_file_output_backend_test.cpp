#include <semilive/publisher/infrastructure/output/h264_file_output_backend.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace model = semilive::publisher::model;
namespace output = semilive::publisher::contracts::output;

using semilive::publisher::infra::output::H264FileOutputBackend;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

class TemporaryOutputFile {
public:
    TemporaryOutputFile() {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::current_path() /
                ("semilive_h264_file_output_" + std::to_string(nonce) +
                 ".h264");
    }

    ~TemporaryOutputFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryOutputFile(const TemporaryOutputFile&) = delete;
    TemporaryOutputFile& operator=(const TemporaryOutputFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

model::EncodedVideoAccessUnit access_unit(std::vector<std::byte> bytes,
                                          const std::uint64_t sequence) {
    return {
        std::move(bytes),
        std::chrono::milliseconds{sequence * 33},
        sequence == 1,
        sequence,
        std::chrono::steady_clock::now(),
    };
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    require(input.is_open(), "output file must be readable");
    const std::vector<char> chars{std::istreambuf_iterator<char>{input},
                                  std::istreambuf_iterator<char>{}};

    std::vector<std::byte> bytes;
    bytes.reserve(chars.size());
    for (const char value : chars) {
        bytes.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(value)));
    }
    return bytes;
}

void writes_annex_b_access_units_in_order() {
    TemporaryOutputFile output_file;
    H264FileOutputBackend backend{output_file.path()};
    const auto first = access_unit(
        {std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x67}},
        1);
    const auto second = access_unit(
        {std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x65},
         std::byte{0x7f}},
        2);

    const auto opened = backend.open();
    const auto first_receipt = backend.consume(first);
    const auto second_receipt = backend.consume(second);
    const auto flushed = backend.flush();
    backend.close();
    backend.close();

    require(opened && opened->output_name == output_file.path().string(),
            "open must report the configured output path");
    require(first_receipt && first_receipt->emitted_units == 1 &&
                first_receipt->emitted_bytes == first.annex_b.size(),
            "consume must report the first complete file unit");
    require(second_receipt && second_receipt->emitted_units == 1 &&
                second_receipt->emitted_bytes == second.annex_b.size(),
            "consume must report the second complete file unit");
    require(flushed && flushed->emitted_units == 0 &&
                flushed->emitted_bytes == 0,
            "file flush must not report additional output bytes");

    auto expected = first.annex_b;
    expected.insert(expected.end(), second.annex_b.begin(),
                    second.annex_b.end());
    require(read_bytes(output_file.path()) == expected,
            "file bytes must be the exact ordered Annex-B concatenation");
}

void reopen_truncates_the_previous_session() {
    TemporaryOutputFile output_file;
    H264FileOutputBackend backend{output_file.path()};
    const auto first = access_unit(
        {std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x67}},
        1);
    const auto replacement = access_unit(
        {std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x65}},
        2);

    require(backend.open().has_value(), "first session must open");
    require(backend.consume(first).has_value(), "first session must write");
    backend.close();

    require(backend.open().has_value(), "closed backend must reopen");
    require(backend.consume(replacement).has_value(),
            "reopened backend must write");
    require(backend.flush().has_value(), "reopened backend must flush");
    backend.close();

    require(read_bytes(output_file.path()) == replacement.annex_b,
            "reopening must truncate bytes from the previous session");
}

void lifecycle_and_input_errors_are_structured() {
    TemporaryOutputFile output_file;
    H264FileOutputBackend backend{output_file.path()};
    const auto valid = access_unit({std::byte{0x01}}, 1);

    const auto consume_while_closed = backend.consume(valid);
    const auto flush_while_closed = backend.flush();
    require(!consume_while_closed &&
                consume_while_closed.error().operation ==
                    output::VideoOutputOperation::State,
            "consume while closed must report a state issue");
    require(!flush_while_closed &&
                flush_while_closed.error().operation ==
                    output::VideoOutputOperation::State,
            "flush while closed must report a state issue");

    require(backend.open().has_value(), "backend must open for lifecycle test");
    const auto duplicate_open = backend.open();
    require(!duplicate_open &&
                duplicate_open.error().operation ==
                    output::VideoOutputOperation::State,
            "duplicate open must report a state issue");

    const auto empty = access_unit({}, 2);
    const auto empty_result = backend.consume(empty);
    require(!empty_result &&
                empty_result.error().operation ==
                    output::VideoOutputOperation::Consume,
            "empty access unit must report a consume issue");
    const auto consume_after_failure = backend.consume(valid);
    require(!consume_after_failure &&
                consume_after_failure.error().operation ==
                    output::VideoOutputOperation::State,
            "failed session must require close before reuse");

    backend.close();
    require(backend.open().has_value(),
            "close must reset a failed backend for a new session");
    backend.close();
}

void invalid_paths_fail_without_creating_parents() {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    const auto missing_parent = std::filesystem::current_path() /
                                ("semilive_missing_output_parent_" +
                                 std::to_string(nonce));
    const auto output_path = missing_parent / "video.h264";
    H264FileOutputBackend backend{output_path};

    const auto opened = backend.open();
    require(!opened &&
                opened.error().operation == output::VideoOutputOperation::Open,
            "a missing parent directory must report an open issue");
    require(!std::filesystem::exists(missing_parent),
            "file backend must not create missing parent directories");
    backend.close();

    H264FileOutputBackend empty_path_backend{std::filesystem::path{}};
    const auto empty_path = empty_path_backend.open();
    require(!empty_path &&
                empty_path.error().operation ==
                    output::VideoOutputOperation::Open,
            "empty output path must report an open issue");
}

}  // namespace

int main() {
    try {
        writes_annex_b_access_units_in_order();
        reopen_truncates_the_previous_session();
        lifecycle_and_input_errors_are_structured();
        invalid_paths_fail_without_creating_parents();
    } catch (const std::exception& error) {
        std::cerr << "H.264 file output backend test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "H.264 file output backend tests passed\n";
    return EXIT_SUCCESS;
}
