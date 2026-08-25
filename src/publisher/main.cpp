#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void print_help() {
    std::cout << "SemiLive publisher (project skeleton)\n"
                 "Usage: semilive_publisher [--help|--version]\n";
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
            std::cout << "semilive_publisher 0.1.0-dev\n";
            return EXIT_SUCCESS;
        }
    }

    if (argc != 1) {
        std::cerr << "Unknown arguments. Use --help for usage.\n";
        return EXIT_FAILURE;
    }

    std::cout << "SemiLive publisher skeleton: implementation pending.\n";
    return EXIT_SUCCESS;
}

