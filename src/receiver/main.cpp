#include <semilive/receiver/composition/receiver_composition.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void print_help() {
    std::cout
        << "SemiLive H.264/RTP receiver\n"
           "Usage: semilive_receiver [--help|--version]\n\n"
           "Command-line receive options will be added in the next "
           "milestone.\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view argument{argv[1]};
        if (argument == "--help") {
            print_help();
            return EXIT_SUCCESS;
        }
        if (argument == "--version") {
            std::cout << "semilive_receiver 0.1.0-dev\n";
            return EXIT_SUCCESS;
        }
    }

    if (argc != 1) {
        std::cerr << "Unknown arguments. Use --help for usage.\n";
        return EXIT_FAILURE;
    }

    semilive::receiver::composition::ReceiverComposition graph;
    const auto assembled = graph.assemble();
    if (!assembled) {
        std::cerr << "Failed to assemble receiver: "
                  << assembled.error().message << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Receiver graph assembled successfully.\n";
    if (const auto disposed = graph.dispose(); !disposed) {
        std::cerr << "Failed to dispose receiver: "
                  << disposed.error().message << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
