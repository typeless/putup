// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/commands.hpp"

#include <cstdlib>
#include <filesystem>

#include <fmt/core.h>

namespace pup::cli {

namespace {

auto const PUP_DIR = ".pup";

} // namespace

auto cmd_init(Options const& /*opts*/) -> int
{
    auto root = std::filesystem::path { std::filesystem::current_path() };

    auto pup_dir = std::filesystem::path { root / PUP_DIR };
    if (std::filesystem::exists(pup_dir)) {
        fmt::print("Already initialized in \"{}\"\n", root.string());
        return EXIT_SUCCESS;
    }

    std::filesystem::create_directory(pup_dir);
    fmt::print("Initialized pup in \"{}\"\n", root.string());
    return EXIT_SUCCESS;
}

} // namespace pup::cli
