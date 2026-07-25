// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "e2e_fixture.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/file_io.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

auto exe_path = std::string {};

auto absolute_path_of(std::string_view launched_as) -> std::string
{
    if (pup::path::is_absolute(launched_as)) {
        return std::string { launched_as };
    }
    auto cwd = pup::platform::current_directory();
    if (!cwd) {
        return std::string { launched_as };
    }
    auto& pool = pup::global_pool();
    return std::string { pool.get(pup::path::join(pool.get(*cwd), launched_as)) };
}

} // namespace

namespace pup::test {

auto test_executable() -> std::string_view { return exe_path; }

} // namespace pup::test

auto main(int argc, char** argv) -> int
{
    if (argc >= 2 && std::string_view { argv[1] } == "--dump-argv") {
        for (auto i = 2; i < argc; ++i) {
            std::printf("%s\n", argv[i]);
        }
        return 0;
    }

    if (argc >= 3 && std::string_view { argv[1] } == "--exit-code") {
        return static_cast<int>(std::strtol(argv[2], nullptr, 10));
    }


    // Resolved before any test can change the working directory.
    exe_path = absolute_path_of(argv[0]);

    return Catch::Session().run(argc, argv);
}
