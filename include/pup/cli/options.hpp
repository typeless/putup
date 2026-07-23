// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <cstddef>
#include <utility>

namespace pup::cli {

/// Convention-checking level for `parse` (see `--check`).
///   None  - skip checks entirely
///   Warn  - run checks, print diagnostics, always exit 0
///   Error - run checks, print diagnostics, exit non-zero on any error
enum class CheckLevel : unsigned char { None,
                                        Warn,
                                        Error };

/// Command-line options
struct Options {
    std::size_t jobs = 0;
    /// Config variable overrides from -D flags
    /// Key is stripped name (e.g., "CC" not "CONFIG_CC")
    Vec<std::pair<StringId, StringId>> config_defines = {};
    bool keep_going = false;
    bool no_stat_cache = false;
    bool verbose = false;
    bool dry_run = false;
    bool stat = false;
    bool all = false; // Force full project build (ignore cwd scoping)
    bool version = false;
    bool help = false;
    bool summary = false;
    bool include_all_deps = false;
    StringId command = StringId::Empty;
    StringId show_format = StringId::Empty;
    StringId show_var_filter = StringId::Empty;
    StringId source_dir = StringId::Empty;
    StringId config_dir = StringId::Empty;
    Vec<StringId> build_dirs = {};
    Vec<StringId> targets = {};
    Vec<StringId> output_targets = {};
    /// Exclude patterns from -x flags (.pupignore syntax, applied to directories)
    Vec<StringId> excludes = {};
    bool show_json = false;
    CheckLevel check = CheckLevel::Warn;
    StringId config_file = StringId::Empty;
};

/// Parse command-line arguments
[[nodiscard]]
auto parse_args(int argc, char** argv) -> Options;

/// Print usage information to stdout
auto print_usage() -> void;

/// Print version information to stdout
auto print_version() -> void;

} // namespace pup::cli
