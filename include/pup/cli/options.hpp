// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pup::cli {

/// Command-line options
struct Options {
    std::size_t jobs = 0;
    bool keep_going = false;
    bool verbose = false;
    bool dry_run = false;
    bool stats = false;
    bool version = false;
    bool help = false;
    bool summary = false;
    bool include_all_deps = false;
    std::string command = {};
    std::string export_format = {};
    std::string source_dir = {};
    std::string build_dir = {};
    std::vector<std::string> targets = {};
};

/// Parse command-line arguments
[[nodiscard]] auto parse_args(int argc, char** argv) -> Options;

/// Print usage information to stdout
auto print_usage() -> void;

/// Print version information to stdout
auto print_version() -> void;

} // namespace pup::cli
