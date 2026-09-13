#include <semilive/receiver/composition/receiver_composition.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void print_help() {
    std::cout
        << "SemiLive H.264/RTP receiver (lifecycle skeleton)\n"
           "Usage: semilive_receiver [--help|--version]\n\n"
           "UDP/RTP and media output options will be added in the next "
           "milestones.\n";
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
        std::cerr << "Failed to assemble receiver skeleton: "
                  << assembled.error().message << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Receiver lifecycle skeleton assembled successfully.\n"
                 "UDP/RTP media receiving is not implemented yet.\n";
    if (const auto disposed = graph.dispose(); !disposed) {
        std::cerr << "Failed to dispose receiver skeleton: "
                  << disposed.error().message << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
