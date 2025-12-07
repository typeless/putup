// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/core/hash.hpp"
#include "pup/core/platform.hpp"
#include "pup/core/result.hpp"
#include "pup/core/types.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

auto main(int argc, char** argv) -> int
{
    std::cout << "pup - Tup build system reimplementation\n";
    std::cout << "Platform: " << pup::PLATFORM << "\n";
    std::cout << "Architecture: " << pup::ARCH << "\n";

    // Test SHA-256 hashing
    auto const test_data = std::string_view{"Hello, pup!"};
    auto const hash = pup::sha256(test_data);
    std::cout << "SHA-256(\"" << test_data << "\"): " << pup::hash_to_hex(hash) << "\n";

    if (argc < 2) {
        std::cout << "\nUsage: pup <command>\n";
        std::cout << "Commands:\n";
        std::cout << "  init    Initialize .pup directory\n";
        std::cout << "  parse   Parse Tupfiles\n";
        std::cout << "  build   Execute build\n";
        return EXIT_SUCCESS;
    }

    auto const command = std::string_view{argv[1]};

    if (command == "init") {
        std::cout << "TODO: Initialize .pup directory\n";
    } else if (command == "parse") {
        std::cout << "TODO: Parse Tupfiles\n";
    } else if (command == "build") {
        std::cout << "TODO: Execute build\n";
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
