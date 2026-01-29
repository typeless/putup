// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace pup::cli {

/// Command-line options
struct Options {
    std::size_t jobs = 0;
    /// Config variable overrides from -D flags
    /// Key is stripped name (e.g., "CC" not "CONFIG_CC")
    std::vector<std::pair<std::string, std::string>> config_defines = {};
    bool keep_going = false;
    bool verbose = false;
    bool dry_run = false;
    bool stat = false;
    bool all = false; // Force full project build (ignore cwd scoping)
    bool version = false;
    bool help = false;
    bool summary = false;
    bool include_all_deps = false;
    std::string command = {};
    std::string show_format = {};
    std::string show_var_filter = {};
    std::string source_dir = {};
    std::string config_dir = {};
    std::vector<std::string> build_dirs = {};
    std::vector<std::string> targets = {};
    std::vector<std::string> output_targets = {};
    bool show_json = false;
    std::string config_file = {}; // path to config file for --config
};

/// Parse command-line arguments
[[nodiscard]]
auto parse_args(int argc, char** argv) -> Options;

/// Print usage information to stdout
auto print_usage() -> void;

/// Print version information to stdout
auto print_version() -> void;

} // namespace pup::cli
