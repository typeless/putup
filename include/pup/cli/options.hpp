// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string.hpp"
#include "pup/core/vec.hpp"

#include <cstddef>
#include <utility>

namespace pup::cli {

/// Command-line options
struct Options {
    std::size_t jobs = 0;
    /// Config variable overrides from -D flags
    /// Key is stripped name (e.g., "CC" not "CONFIG_CC")
    Vec<std::pair<String, String>> config_defines = {};
    bool keep_going = false;
    bool verbose = false;
    bool dry_run = false;
    bool stat = false;
    bool all = false; // Force full project build (ignore cwd scoping)
    bool version = false;
    bool help = false;
    bool summary = false;
    bool include_all_deps = false;
    String command = {};
    String show_format = {};
    String show_var_filter = {};
    String source_dir = {};
    String config_dir = {};
    Vec<String> build_dirs = {};
    Vec<String> targets = {};
    Vec<String> output_targets = {};
    bool show_json = false;
    String config_file = {}; // path to config file for --config
};

/// Parse command-line arguments
[[nodiscard]]
auto parse_args(int argc, char** argv) -> Options;

/// Print usage information to stdout
auto print_usage() -> void;

/// Print version information to stdout
auto print_version() -> void;

} // namespace pup::cli
