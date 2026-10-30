#include <iostream>
#include <string_view>

#include "axon/version.hpp"

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << "axon " << axon::version() << '\n';
        return 0;
    }
    std::cerr << "usage: axon --version\n";
    return 2;
}
