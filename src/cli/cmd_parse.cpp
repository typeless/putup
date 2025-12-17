// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/types.hpp"
#include "pup/graph/dag.hpp"

#include <cstdlib>

#include <fmt/core.h>

namespace pup::cli {

namespace {

auto parse_single_variant(Options const& opts, std::string_view variant_name) -> int
{
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        fmt::print(stderr, "[{}] Error: {}\n", variant_name, result.error().message);
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    if (opts.verbose) {
        fmt::print("[{}] Project root: \"{}\"\n", variant_name, ctx.layout().source_root.string());
        fmt::print("[{}] Tupfiles:\n", variant_name);
        for (auto const& dir : ctx.parsed_dirs()) {
            auto tupfile_path = (dir == "." || dir.empty())
                ? ctx.layout().source_root / "Tupfile"
                : ctx.layout().source_root / dir / "Tupfile";
            fmt::print("[{}]   {}\n", variant_name, tupfile_path.string());
        }
    }

    auto commands = ctx.graph().nodes_of_type(pup::NodeType::Command);

    if (opts.verbose && !commands.empty()) {
        fmt::print("[{}] Commands:\n", variant_name);
        for (auto id : commands) {
            if (auto const* node = ctx.graph().get_node(id)) {
                fmt::print("[{}]   {}\n", variant_name, node->display.empty() ? node->command : node->display);
            }
        }
    }

    fmt::print("[{}] Parsed {} Tupfile(s), {} commands\n", variant_name, ctx.parsed_dirs().size(), commands.size());

    return EXIT_SUCCESS;
}

} // namespace

auto cmd_parse(Options const& opts) -> int
{
    return for_each_variant(opts, parse_single_variant, "Parsing");
}

} // namespace pup::cli
