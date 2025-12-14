// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/types.hpp"
#include "pup/graph/dag.hpp"

#include <cstdlib>

#include <fmt/core.h>

namespace pup::cli {

auto cmd_parse(Options const& opts) -> int
{
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        fmt::print(stderr, "Error: {}\n", result.error().message);
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    if (opts.verbose) {
        fmt::print("Project root: \"{}\"\n", ctx.layout().source_root.string());
        fmt::print("\nTupfiles:\n");
        for (auto const& dir : ctx.parsed_dirs()) {
            auto tupfile_path = (dir == "." || dir.empty())
                ? ctx.layout().source_root / "Tupfile"
                : ctx.layout().source_root / dir / "Tupfile";
            fmt::print("  {}\n", tupfile_path.string());
        }
    }

    auto commands = ctx.graph().nodes_of_type(pup::NodeType::Command);

    if (opts.verbose && !commands.empty()) {
        fmt::print("\nCommands:\n");
        for (auto id : commands) {
            if (auto const* node = ctx.graph().get_node(id)) {
                auto display = node->display.empty() ? node->command : node->display;
                fmt::print("  {}\n", display);
            }
        }
    }

    fmt::print("Parsed {} Tupfile(s), {} commands\n", ctx.parsed_dirs().size(), commands.size());

    return EXIT_SUCCESS;
}

} // namespace pup::cli
