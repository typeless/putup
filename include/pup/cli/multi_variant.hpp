// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/cli/options.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/function.hpp"
#include "pup/core/print.hpp"

#include <string_view>

namespace pup::cli {

/// Handler for single-variant operations
/// @param opts Options with build_dirs containing exactly one variant
/// @param variant_name Name of the variant (for prefixed output), empty for in-tree builds
/// @return EXIT_SUCCESS or EXIT_FAILURE
using VariantHandler = Function<int(Options const&, std::string_view)>;

/// Run a command for each variant (parallel when multiple)
///
/// Orchestration logic:
/// 1. Discover source root from opts
/// 2. Determine variants (explicit -B flags or auto-detect)
/// 3. Single variant or in-tree: call handler directly
/// 4. Multiple variants: fork child processes, aggregate results
///
/// @param opts Command-line options (may have multiple -B flags)
/// @param handler Function to execute for each variant
/// @param command_name Name for verbose output (e.g., "Building", "Cleaning")
/// @return EXIT_SUCCESS if all variants succeed, EXIT_FAILURE otherwise
[[nodiscard]]
auto for_each_variant(
    Options const& opts,
    VariantHandler handler,
    std::string_view command_name = "Processing"
) -> int;

/// Write one `[variant] `-prefixed line in a single write, so that variants built in parallel
/// never interleave inside a line.
template<typename... Args>
auto write_variant_line(Stream stream, std::string_view variant_name, std::string_view pattern, Args const&... args) -> void
{
    auto line = Buf {};
    line += '[';
    line.append(variant_name);
    line.append("] ");
    if constexpr (sizeof...(Args) == 0) {
        line.append(pattern);
    } else {
        FormatArg arg_array[] = { FormatArg(args)... };
        format_to(line, pattern, arg_array, sizeof...(Args));
    }
    write_to(stream, line.view());
}

/// Print a `[variant] `-prefixed line to stdout.
template<typename... Args>
auto vprint(std::string_view variant_name, std::string_view pattern, Args const&... args) -> void
{
    write_variant_line(Stream::Out, variant_name, pattern, args...);
}

/// Print a `[variant] `-prefixed line to stderr.
template<typename... Args>
auto veprint(std::string_view variant_name, std::string_view pattern, Args const&... args) -> void
{
    write_variant_line(Stream::Err, variant_name, pattern, args...);
}

} // namespace pup::cli
