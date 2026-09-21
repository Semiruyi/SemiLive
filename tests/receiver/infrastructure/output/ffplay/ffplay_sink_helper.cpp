#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char* argv[]) {
    std::filesystem::path output_path;
    std::chrono::milliseconds initial_delay{};
    bool exit_immediately = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--output" && ++index < argc) {
            output_path = argv[index];
            continue;
        }
        if (argument == "--initial-delay-ms" && ++index < argc) {
            std::uint64_t value = 0;
            const std::string_view text{argv[index]};
            const auto parsed = std::from_chars(
                text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != text.data() + text.size()) {
                return 2;
            }
            initial_delay = std::chrono::milliseconds{value};
            continue;
        }
        if (argument == "--exit-immediately") {
            exit_immediately = true;
            continue;
        }
    }
    if (exit_immediately) {
        return 0;
    }
    if (output_path.empty()) {
        return 2;
    }

#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
        return 3;
    }
#endif

    std::this_thread::sleep_for(initial_delay);
    std::ofstream output{output_path, std::ios::binary | std::ios::trunc};
    if (!output) {
        return 4;
    }
    output << std::cin.rdbuf();
    output.flush();
    return output ? 0 : 5;
}
